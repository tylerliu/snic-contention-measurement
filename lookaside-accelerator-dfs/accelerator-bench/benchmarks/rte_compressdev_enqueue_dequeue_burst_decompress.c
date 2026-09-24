#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_compressdev.h>

#include "driver/benchmark_driver.h"

// Define constants
#define MBUF_DATA_SIZE 32768  // Large mbuf size for compression data
#define MBUF_POOL_SIZE 8192
#define MBUF_CACHE_SIZE 128

static unsigned int burst_size;
static unsigned int num_queues = 1;
struct rte_comp_op *ops[256];

// Mempool and mbufs used by this benchmark
static struct rte_mempool *mbuf_pool;
static struct rte_mbuf *mbufs[256];
static struct rte_mbuf *dst_mbufs[256];

// Tunables for mbuf pool and mbuf payload sizes
#define MBUF_POOL_SIZE 16384
#define MBUF_CACHE_SIZE 256
#define MBUF_DATA_SIZE 32768

// Variables for teardown and metadata
static void *new_decomp_private_xform = NULL;
static const char *algorithm = NULL;
static const char *checksum = NULL;

static uint8_t cdev_id = 0;
static void *comp_private_xform;
static void *decomp_private_xform;
static struct rte_mempool *comp_op_pool;

// Queue descriptor count (compile-time tunable)
#ifndef NB_DESCRIPTORS
#define NB_DESCRIPTORS 128
#endif
#if NB_DESCRIPTORS > 256
#error "NB_DESCRIPTORS must be <= 256 for this benchmark"
#endif

// Free list of ops for enqueueing
static struct rte_comp_op *free_ops[256];
static unsigned int free_head = 0; // index of next pop
static unsigned int free_tail = 0; // index of next push
static unsigned int free_count = 0; // number available

static inline void free_ops_push(struct rte_comp_op *op) {
    free_ops[free_tail] = op;
    free_tail = (free_tail + 1) % NB_DESCRIPTORS;
    free_count++;
}

static inline unsigned int free_ops_pop_bulk(struct rte_comp_op **out, unsigned int max_pop) {
    unsigned int n = (free_count < max_pop) ? free_count : max_pop;
    for (unsigned int i = 0; i < n; i++) {
        out[i] = free_ops[free_head];
        free_head = (free_head + 1) % NB_DESCRIPTORS;
    }
    free_count -= n;
    return n;
}

// Compression constants
#define MAX_COMPRESSED_SIZE 2048
#define COMPRESS_LEVEL 6
#define COMPRESS_WINDOW_SIZE 15

// Global xform used by benchmarks (device supports decompression only)
struct rte_comp_xform comp_xform = {
	.type = RTE_COMP_DECOMPRESS,
	.decompress = {
		.algo = RTE_COMP_ALGO_DEFLATE,
		.chksum = RTE_COMP_CHECKSUM_CRC32,
	}
};

void setup_compressdev() {
    // Read num_queues parameter early as it is needed for configuration
    const char* num_queues_str = get_benchmark_param("num_queues");
    num_queues = num_queues_str ? (unsigned int)strtoul(num_queues_str, NULL, 10) : 1;
    if (num_queues < 1) num_queues = 1;

    // Check that compression device is available
    int num_comp_devices = rte_compressdev_count();
    if (num_comp_devices < 1) {
        rte_exit(EXIT_FAILURE, "No compression devices available\n");
    }

    // Get compression device info
    struct rte_compressdev_info cdev_info;
    rte_compressdev_info_get(cdev_id, &cdev_info);

    // Create compression operation pool
    comp_op_pool = rte_comp_op_pool_create("comp_op_pool", 
                                         8192, 128, 0, rte_socket_id());
    if (comp_op_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to create compression operation pool\n");
    }

    // Note: Private xforms are allocated directly by the device, no separate pool needed

    // Configure compression device
    struct rte_compressdev_config config = {
        .nb_queue_pairs = (uint16_t)num_queues,
        .socket_id = rte_socket_id(),
    };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (rte_compressdev_configure(cdev_id, &config) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to configure compressdev %u\n", cdev_id);
    }
#pragma GCC diagnostic pop

    // Setup queue pair with maximum inflight operations
    for (unsigned int q = 0; q < num_queues; q++) {
        if (rte_compressdev_queue_pair_setup(cdev_id, q, 128, rte_socket_id()) < 0) {
            rte_exit(EXIT_FAILURE, "Failed to setup queue pair %u\n", q);
        }
    }

    // Start compression device
    if (rte_compressdev_start(cdev_id) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to start compression device\n");
    }

    // Create private xforms
    if (rte_compressdev_private_xform_create(cdev_id, &comp_xform, &decomp_private_xform) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to create decompression private xform\n");
    }
}

void setup_benchmark() {
    const char* burst_size_str = get_benchmark_param("burst_size");
    burst_size = burst_size_str ? (unsigned int)strtoul(burst_size_str, NULL, 10) : 32;

    // Optional parameter: data size per operation
const char* data_size_str = get_benchmark_param("data_size");
unsigned int data_size = data_size_str ? (unsigned int)strtoul(data_size_str, NULL, 10) : 1024;
if (data_size > MBUF_DATA_SIZE) {
    rte_exit(EXIT_FAILURE, "data_size (%u) exceeds MBUF_DATA_SIZE (%u)", data_size, (unsigned)MBUF_DATA_SIZE);
}

// Algorithm parameter: deflate, lz4, null
const char* algorithm_str = get_benchmark_param("algorithm");
algorithm = algorithm_str ? algorithm_str : "deflate";

// Checksum parameter: none, crc32, adler32, xxhash32
const char* checksum_str = get_benchmark_param("checksum");
checksum = checksum_str ? checksum_str : "none";

// Window size parameter: 1024, 2048, 4096, 8192, 16384, 32768
const char* window_size_str = get_benchmark_param("window_size");
unsigned int window_size = window_size_str ? (unsigned int)strtoul(window_size_str, NULL, 10) : 32768;

// Configure decompression xform based on parameters
struct rte_comp_xform decomp_xform = {
    .type = RTE_COMP_DECOMPRESS,
};

// Set algorithm
if (strcmp(algorithm, "deflate") == 0) {
    decomp_xform.decompress.algo = RTE_COMP_ALGO_DEFLATE;
} else if (strcmp(algorithm, "lz4") == 0) {
    decomp_xform.decompress.algo = RTE_COMP_ALGO_LZ4;
} else if (strcmp(algorithm, "null") == 0) {
    decomp_xform.decompress.algo = RTE_COMP_ALGO_NULL;
} else {
    rte_exit(EXIT_FAILURE, "Unsupported algorithm: %s", algorithm);
}

// Set checksum type
if (strcmp(checksum, "crc32") == 0) {
    decomp_xform.decompress.chksum = RTE_COMP_CHECKSUM_CRC32;
} else if (strcmp(checksum, "adler32") == 0) {
    decomp_xform.decompress.chksum = RTE_COMP_CHECKSUM_ADLER32;
} else if (strcmp(checksum, "xxhash32") == 0) {
    decomp_xform.decompress.chksum = RTE_COMP_CHECKSUM_XXHASH32;
} else {
    decomp_xform.decompress.chksum = RTE_COMP_CHECKSUM_NONE;
}

// Create new private xform for this specific configuration
if (rte_compressdev_private_xform_create(cdev_id, &decomp_xform, &new_decomp_private_xform) < 0) {
    rte_exit(EXIT_FAILURE, "Failed to create decompression private xform for algorithm %s", algorithm);
}

// Allocate ops up to NB_DESCRIPTORS for pipeline use
if (rte_comp_op_bulk_alloc(comp_op_pool, ops, NB_DESCRIPTORS) < 0) {
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
if (rte_pktmbuf_alloc_bulk(mbuf_pool, dst_mbufs, NB_DESCRIPTORS) < 0) {
    rte_exit(EXIT_FAILURE, "Failed to allocate dst mbufs");
}

// Load pre-compressed data files
const char* data_dir = get_benchmark_param("data_dir");
if (data_dir == NULL) {
    data_dir = "compressed_data";  // Default directory
}

// Create filename based on algorithm and data size
char filename[256];
snprintf(filename, sizeof(filename), "%s/%s_text_%u.bin", data_dir, algorithm, data_size);

FILE *compressed_file = fopen(filename, "rb");
if (compressed_file == NULL) {
    rte_exit(EXIT_FAILURE, "Failed to open compressed data file: %s\n", filename);
}

// Get file size
fseek(compressed_file, 0, SEEK_END);
long file_size = ftell(compressed_file);
fseek(compressed_file, 0, SEEK_SET);

if (file_size > MBUF_DATA_SIZE) {
    fclose(compressed_file);
    rte_exit(EXIT_FAILURE, "Compressed file size (%ld) exceeds MBUF_DATA_SIZE (%u)\n", file_size, (unsigned)MBUF_DATA_SIZE);
}

// Read compressed data
uint8_t *compressed_data = malloc(file_size);
if (fread(compressed_data, 1, file_size, compressed_file) != file_size) {
    free(compressed_data);
    fclose(compressed_file);
    rte_exit(EXIT_FAILURE, "Failed to read compressed data from %s", filename);
}
fclose(compressed_file);

// Initialize mbufs with actual compressed data
for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
    rte_pktmbuf_reset(mbufs[i]);
    rte_pktmbuf_append(mbufs[i], file_size);
    
    // Copy compressed data to mbuf
    uint8_t *data = rte_pktmbuf_mtod(mbufs[i], uint8_t *);
    memcpy(data, compressed_data, file_size);
    
    // Initialize destination mbufs
    rte_pktmbuf_reset(dst_mbufs[i]);
    rte_pktmbuf_append(dst_mbufs[i], data_size);  // Reserve space for decompressed data
}

free(compressed_data);

// Setup decompression operations
for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
    struct rte_comp_op *op = ops[i];
    op->m_src = mbufs[i];
    op->m_dst = dst_mbufs[i]; // Allocate destination mbuf
    
    op->src.offset = 0;
    op->src.length = file_size;  // Use actual compressed data size
    op->dst.offset = 0;
    
    // Set private xform in the operation
    op->private_xform = new_decomp_private_xform;
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
    unsigned int inflight = 0;
    uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t duration_cycles = (uint64_t)g_duration * tsc_hz;
    unsigned long long total_ops = 0;

    start = rte_rdtsc();
    uint64_t end_time = start + duration_cycles;
    
    while (rte_rdtsc() < end_time) {
        do {
            // Dequeue any available completions up to burst_size
            struct rte_comp_op *deq_ops[256];
            unsigned int deq = rte_compressdev_dequeue_burst(cdev_id, 0, deq_ops, burst_size);
            if (deq > 0) {
                inflight -= deq;
                for (unsigned int j = 0; j < deq; j++) {
                    free_ops_push(deq_ops[j]);
                }
            }
        } while (inflight > NB_DESCRIPTORS - burst_size && rte_rdtsc() < end_time); // Ensure we have enough free ops

        if (rte_rdtsc() >= end_time) break;

        int actual_burst_size = burst_size;

        struct rte_comp_op *to_send[256];
        unsigned int prepared = free_ops_pop_bulk(to_send, actual_burst_size);
        if (prepared != actual_burst_size) {
            fprintf(stderr, "Error: free_ops_pop_bulk returned %u ops but expected %u\n", prepared, burst_size);
            assert(0);
        }
        unsigned int sent = rte_compressdev_enqueue_burst(cdev_id, 0, to_send, actual_burst_size);
        if (sent != actual_burst_size) {
            fprintf(stderr, "Error: rte_compressdev_enqueue_burst sent %u ops but expected %u\n", sent, burst_size);
            assert(0);
        }
        execute_pause_loops(sent);
        inflight += sent;
        total_ops += sent;
    }

    // Drain remaining completions
    while (inflight > 0) {
        struct rte_comp_op *deq_ops[256];
        unsigned int deq = 0;
        for (unsigned int q = 0; q < num_queues; q++) {
            unsigned int n = rte_compressdev_dequeue_burst(cdev_id, q, deq_ops, burst_size);
            if (n > 0) {
                 deq += n;
                 for (unsigned int j = 0; j < n; j++) {
                     free_ops_push(deq_ops[j]);
                 }
            }
        }
        if (deq > 0) inflight -= deq;
    }

    end = rte_rdtsc();
    uint64_t total_cycles = end - start;
    printf("Total cycles: %lu\n", (unsigned long)total_cycles);
    printf("Total operations: %llu\n", total_ops);
    printf("Wait cycles: %lu\n", (unsigned long)get_wait_cycles());
}

void teardown_benchmark() {
    // Free allocated compression operations
    for (unsigned int i = 0; i < NB_DESCRIPTORS; i++) {
        if (ops[i] != NULL) {
            rte_comp_op_free(ops[i]);
            ops[i] = NULL;
        }
        if (mbufs[i] != NULL) {
            rte_pktmbuf_free(mbufs[i]);
            mbufs[i] = NULL;
        }
        if (dst_mbufs[i] != NULL) {
            rte_pktmbuf_free(dst_mbufs[i]);
            dst_mbufs[i] = NULL;
        }
    }

    // Free the dynamically created private xform
    if (new_decomp_private_xform != NULL) {
        rte_compressdev_private_xform_free(cdev_id, new_decomp_private_xform);
        new_decomp_private_xform = NULL;
    }

    // Print metadata
    printf("metadata: {'burst_size': %u, 'nb_descriptors': %u, 'algorithm': '%s', 'checksum': '%s', 'num_queues': %u}\n", 
           burst_size, (unsigned)NB_DESCRIPTORS, algorithm, checksum, num_queues);
}

void teardown_compressdev() {
    // Free private xforms
    if (comp_private_xform != NULL) {
        rte_compressdev_private_xform_free(cdev_id, comp_private_xform);
        comp_private_xform = NULL;
    }
    if (decomp_private_xform != NULL) {
        rte_compressdev_private_xform_free(cdev_id, decomp_private_xform);
        decomp_private_xform = NULL;
    }
    
    // Stop and close compression device
    rte_compressdev_stop(cdev_id);
    rte_compressdev_close(cdev_id);

    // Free compression operation pool
    if (comp_op_pool != NULL) {
        rte_mempool_free(comp_op_pool);
        comp_op_pool = NULL;
    }
}

int main(int argc, char **argv) {
    init_dpdk(argc, argv);
    setup_compressdev();
    setup_benchmark();
    run_benchmark();
    teardown_benchmark();
    teardown_compressdev();
    cleanup_dpdk();
    return 0;
}
