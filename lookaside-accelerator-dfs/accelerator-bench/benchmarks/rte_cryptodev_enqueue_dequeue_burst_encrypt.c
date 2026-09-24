#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>

#include "driver/benchmark_driver.h"

#define MAX_AES_GCM_IV_LENGTH 12

static unsigned int burst_size;
static unsigned int num_queues = 1;
struct rte_crypto_op *ops[256];

// Mempool and mbufs used by this benchmark
static struct rte_mempool *mbuf_pool;
static struct rte_mbuf *mbufs[256];

// Tunables for mbuf pool and mbuf payload sizes
#define MBUF_POOL_SIZE 16384
#define MBUF_CACHE_SIZE 256
#define MBUF_DATA_SIZE 32768

// Per-op inputs
static uint8_t ivs[256][MAX_AES_GCM_IV_LENGTH];

static uint8_t cdev_id = 0;
static struct rte_cryptodev_sym_session *enc_session;
static struct rte_mempool *crypto_op_pool;
static struct rte_mempool *session_pool;

// Queue descriptor count (compile-time tunable)
#ifndef NB_DESCRIPTORS
#define NB_DESCRIPTORS 128
#endif
#if NB_DESCRIPTORS > 256
#error "NB_DESCRIPTORS must be <= 256 for this benchmark"
#endif

// Free list of ops for enqueueing
static struct rte_crypto_op *free_ops[256];
static unsigned int free_head = 0; // index of next pop
static unsigned int free_tail = 0; // index of next push
static unsigned int free_count = 0; // number available

static inline void free_ops_push(struct rte_crypto_op *op) {
    free_ops[free_tail] = op;
    free_tail = (free_tail + 1) % NB_DESCRIPTORS;
    free_count++;
}

static inline unsigned int free_ops_pop_bulk(struct rte_crypto_op **out, unsigned int max_pop) {
    unsigned int n = (free_count < max_pop) ? free_count : max_pop;
    for (unsigned int i = 0; i < n; i++) {
        out[i] = free_ops[free_head];
        free_head = (free_head + 1) % NB_DESCRIPTORS;
    }
    free_count -= n;
    return n;
}

// Crypto constants
#define AES128_KEY_LENGTH 16
#define MAX_AES_GCM_IV_LENGTH 12
#define AES_GCM_TAG_LENGTH 16

void setup_cryptodev() {
    // Read num_queues parameter early as it is needed for configuration
    const char* num_queues_str = get_benchmark_param("num_queues");
    num_queues = num_queues_str ? (unsigned int)strtoul(num_queues_str, NULL, 10) : 1;
    if (num_queues < 1) num_queues = 1;

    // Check that crypto device is available
    int num_crypto_devices = rte_cryptodev_count();
    if (num_crypto_devices < 1) {
        rte_exit(EXIT_FAILURE, "No crypto devices available\n");
    }

    // Get crypto device info
    struct rte_cryptodev_info cdev_info;
    rte_cryptodev_info_get(cdev_id, &cdev_info);

    // Create crypto operation pool
    crypto_op_pool = rte_crypto_op_pool_create("crypto_op_pool", 
                                              RTE_CRYPTO_OP_TYPE_SYMMETRIC, 
                                              8192, 128, MAX_AES_GCM_IV_LENGTH, rte_socket_id());
    if (crypto_op_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to create crypto operation pool\n");
    }

    // Create session pool
    const uint32_t private_session_size = rte_cryptodev_sym_get_private_session_size(cdev_id);
    session_pool = rte_cryptodev_sym_session_pool_create("session_pool",
                                                        8192, 128, private_session_size, 0, rte_socket_id());
    if (session_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to create session pool\n");
    }

    // Configure crypto device
    struct rte_cryptodev_config config = {
        .nb_queue_pairs = (uint16_t)num_queues,
        .socket_id = rte_socket_id(),
        .ff_disable = RTE_CRYPTODEV_FF_SECURITY,
    };
    if (rte_cryptodev_configure(cdev_id, &config) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to configure cryptodev %u\n", cdev_id);
    }

    // Setup queue pairs
    struct rte_cryptodev_qp_conf qp_conf = {
        .nb_descriptors = NB_DESCRIPTORS
    };
    for (unsigned int q = 0; q < num_queues; q++) {
        if (rte_cryptodev_queue_pair_setup(cdev_id, q, &qp_conf, rte_socket_id()) < 0) {
            rte_exit(EXIT_FAILURE, "Failed to setup queue pair %u\n", q);
        }
    }

    // Start crypto device
    if (rte_cryptodev_start(cdev_id) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to start crypto device\n");
    }

    // Create a sample key for the session
    uint8_t key[AES128_KEY_LENGTH];
    for (int i = 0; i < AES128_KEY_LENGTH; i++) {
        key[i] = i; // Simple key for testing
    }

    // Setup AEAD transform for encrypt
    struct rte_crypto_sym_xform enc_xform = {
        .type = RTE_CRYPTO_SYM_XFORM_AEAD,
        .next = NULL,
        .aead = {
            .op = RTE_CRYPTO_AEAD_OP_ENCRYPT,
            .algo = RTE_CRYPTO_AEAD_AES_GCM,
            .key.data = key,
            .key.length = AES128_KEY_LENGTH,
            .iv.offset = sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op), 
            .iv.length = MAX_AES_GCM_IV_LENGTH,
            .aad_length = 0,
            .digest_length = AES_GCM_TAG_LENGTH,
        },
    };

    // Create session
    enc_session = rte_cryptodev_sym_session_create(cdev_id, &enc_xform, session_pool);
    if (enc_session == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to create encrypt session\n");
    }
}

void setup_benchmark() {
    const char* burst_size_str = get_benchmark_param("burst_size");
    burst_size = burst_size_str ? (unsigned int)strtoul(burst_size_str, NULL, 10) : 32;

    // Optional parameter: total data size per packet (includes tag)
    const char* data_size_str = get_benchmark_param("data_size");
    unsigned int data_size = data_size_str ? (unsigned int)strtoul(data_size_str, NULL, 10) : 1024;
    // For encryption, input is plaintext so checking against TAG_LENGTH is not strictly necessary for negative sizing,
    // but we need space for tag in output. MBUF size check is important.
    if (data_size > MBUF_DATA_SIZE) {
        rte_exit(EXIT_FAILURE, "data_size (%u) exceeds MBUF_DATA_SIZE (%u)", data_size, (unsigned)MBUF_DATA_SIZE);
    }
    
    // For encryption we'll treat data_size as the PLAINTEXT size. 
    // The output will be PLAINTEXT + TAG.
    // Ensure we have enough space in mbuf for the result.
    if (data_size + AES_GCM_TAG_LENGTH > MBUF_DATA_SIZE) {
        rte_exit(EXIT_FAILURE, "data_size + tag (%u) exceeds MBUF_DATA_SIZE (%u)", data_size + (unsigned)AES_GCM_TAG_LENGTH, (unsigned)MBUF_DATA_SIZE);
    }

    // Allocate ops up to NB_DESCRIPTORS for pipeline use
    if (rte_crypto_op_bulk_alloc(crypto_op_pool, RTE_CRYPTO_OP_TYPE_SYMMETRIC, ops, NB_DESCRIPTORS) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to allocate ops");
    }

    // Create mbuf pool on first use
    if (mbuf_pool == NULL) {
        mbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", MBUF_POOL_SIZE, MBUF_CACHE_SIZE, 0, MBUF_DATA_SIZE, rte_socket_id());
        if (mbuf_pool == NULL) {
            rte_exit(EXIT_FAILURE, "Failed to create mbuf pool");
        }
    }

    // Allocate mbufs for all ops
    if (rte_pktmbuf_alloc_bulk(mbuf_pool, mbufs, NB_DESCRIPTORS) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to allocate mbufs");
    }


    // Initialize per-op buffers (IVs) and set lengths
    for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
        for (unsigned int j = 0; j < MAX_AES_GCM_IV_LENGTH; j++) {
            ivs[i][j] = (uint8_t)(i + j);
        }

        rte_pktmbuf_reset(mbufs[i]);
        // Fill with dummy plaintext data. Reserve space for tag at end.
        // For in-place, the mbuf data len must include the space where digest is written
        // if we want it to be part of the packet. 
        // We append full size so the memory is allocated and legally accessible within data_len.
        rte_pktmbuf_append(mbufs[i], data_size + AES_GCM_TAG_LENGTH);
    }

    for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
        struct rte_crypto_op *op = ops[i];
        op->sym->m_src = mbufs[i];
        op->sym->m_dst = NULL; // In-place operation

        op->sym->aead.data.offset = 0;
        op->sym->aead.data.length = data_size; // Plaintext length

        // For encryption, digest buffer is where tag is written (usually at end of data in dst mbuf)
        // Here it is at end of src mbuf data.
        op->sym->aead.digest.data = rte_pktmbuf_mtod_offset(mbufs[i], uint8_t *, data_size);
        op->sym->aead.aad.data = rte_pktmbuf_mtod_offset(mbufs[i], uint8_t *, 0); // Dummy AAD

        rte_crypto_op_attach_sym_session(op, enc_session);
    }

    // Initialize free ops queue
    free_head = free_tail = 0;
    free_count = 0;
    for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
        free_ops_push(ops[i]);
    }
}

void run_benchmark(void) {
    uint64_t start, end;
    uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t duration_cycles = (uint64_t)g_duration * tsc_hz;
    unsigned long long total_ops = 0;
    unsigned int inflight = 0;
    unsigned int q_req_idx = 0; // Next queue to enqueue to

    start = rte_rdtsc();
    uint64_t end_time = start + duration_cycles;
    
    while (rte_rdtsc() < end_time) {
        do {
            // Dequeue any available completions up to burst_size
            struct rte_crypto_op *deq_ops[256];
            unsigned int deq = 0;
            // Check all queues
            for (unsigned int k = 0; k < num_queues; k++) {
                 deq += rte_cryptodev_dequeue_burst(cdev_id, k, &deq_ops[deq], burst_size - deq);
                 if (deq >= burst_size) break;
            }
            if (deq > 0) {
                inflight -= deq;
                for (unsigned int j = 0; j < deq; j++) {
                    free_ops_push(deq_ops[j]);
                }
            }
        } while (inflight > NB_DESCRIPTORS - burst_size && rte_rdtsc() < end_time); // Ensure we have enough free ops

        if (rte_rdtsc() >= end_time) break;

        int actual_burst_size = burst_size;

        struct rte_crypto_op *to_send[256];
        unsigned int prepared = free_ops_pop_bulk(to_send, actual_burst_size);
        if (prepared != actual_burst_size) {
            fprintf(stderr, "Error: free_ops_pop_bulk returned %u ops but expected %u\n", prepared, burst_size);
            assert(0);
        }
        unsigned int sent = rte_cryptodev_enqueue_burst(cdev_id, q_req_idx, to_send, actual_burst_size);
        if (sent != actual_burst_size) {
            fprintf(stderr, "Error: rte_cryptodev_enqueue_burst sent %u ops but expected %u\n", sent, burst_size);
            assert(0);
        }
        execute_pause_loops(sent);
        q_req_idx = (q_req_idx + 1) % num_queues;
        inflight += sent;
        total_ops += sent;
    }

    // Drain remaining completions
    while (inflight > 0) {
        struct rte_crypto_op *deq_ops[256];
        unsigned int deq = 0;
        for(unsigned int k=0; k<num_queues; k++) {
             unsigned int n = rte_cryptodev_dequeue_burst(cdev_id, k, &deq_ops[deq], burst_size - deq);
             deq += n;
             if (deq >= burst_size) break;
        }

        if (deq > 0) {
            inflight -= deq;
            for (unsigned int j = 0; j < deq; j++) {
                free_ops_push(deq_ops[j]);
            }
        }
    }

    end = rte_rdtsc();
    uint64_t total_cycles = end - start;
    printf("Total cycles: %lu\n", (unsigned long)total_cycles);
    printf("Total operations: %llu\n", total_ops);
    printf("Wait cycles: %lu\n", (unsigned long)get_wait_cycles());
}

void teardown_benchmark() {
    // Free allocated crypto operations
    for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
        if (ops[i] != NULL) {
            rte_crypto_op_free(ops[i]);
            ops[i] = NULL;
        }
        if (mbufs[i] != NULL) {
            rte_pktmbuf_free(mbufs[i]);
            mbufs[i] = NULL;
        }
    }

    // Print metadata
    printf("metadata: {'burst_size': %u, 'nb_descriptors': %u, 'num_queues': %u}\n", burst_size, (unsigned)NB_DESCRIPTORS, num_queues);
}

void teardown_cryptodev() {
    // Free sessions
    if (enc_session != NULL) {
        rte_cryptodev_sym_session_free(cdev_id, enc_session);
        enc_session = NULL;
    }
    
    // Stop and close crypto device
    rte_cryptodev_stop(cdev_id);
    rte_cryptodev_close(cdev_id);

    // Free crypto operation pool
    if (crypto_op_pool != NULL) {
        rte_mempool_free(crypto_op_pool);
        crypto_op_pool = NULL;
    }

    // Free session pool
    if (session_pool != NULL) {
        rte_mempool_free(session_pool);
        session_pool = NULL;
    }
}

int main(int argc, char **argv) {
    init_dpdk(argc, argv);
    setup_cryptodev();
    setup_benchmark();
    run_benchmark();
    teardown_benchmark();
    teardown_cryptodev();
    cleanup_dpdk();
    return 0;
}
