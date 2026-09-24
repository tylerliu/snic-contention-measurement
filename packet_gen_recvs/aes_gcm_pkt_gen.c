#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <getopt.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <linux/if_packet.h>
#include <pthread.h>
#include <stdatomic.h>

#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282
#define AES_BLOCK_SIZE 16  // 128 bits
#define AES_IV_SIZE 12   // 96 bits
#define AES_TAG_SIZE 12  // 96 bits
#define MAX_DATA_SIZE 1280  // Maximum data size in bytes
#define MTU_SIZE 1500    // Maximum Transmission Unit size
#define MAX_PACKET_SIZE (AES_IV_SIZE + MAX_DATA_SIZE + AES_TAG_SIZE)  // Maximum total packet size
#define DEFAULT_RATE 10  // Default sending rate in packets per second
#define DEFAULT_SOURCE_PORTS 1  // Default number of source ports
#define MIN_SOURCE_PORT 1024   // Minimum source port number
#define MAX_SOURCE_PORT 65535  // Maximum source port number
#define HIGH_RATE_THRESHOLD 100  // Rate above which to show only statistics
#define STATS_INTERVAL 1  // Statistics display interval in seconds
#define BATCH_SIZE 64     // Number of packets to send in a batch
#define DEFAULT_THREADS 1  // Default number of worker threads (single-threaded)

// Batch sending structures
struct packet_info {
    unsigned char data[MAX_PACKET_SIZE];
    int size;
    int socket_index;
};

// Per-thread statistics for lock-free updates
struct thread_stats {
    atomic_ullong packets_sent;
    atomic_ullong bytes_sent;
    atomic_ullong packets_this_second;
    atomic_ullong bytes_this_second;
};

// Thread context for multi-threaded mode
struct thread_context {
    int thread_id;
    int *sockets;
    int num_sockets;
    struct sockaddr_in *dest_addr;
    const char *dest_ip;
    int dest_port;
    int show_stats_only;
    EVP_CIPHER_CTX *ctx;
    unsigned char *key;
    struct thread_stats *stats;
    int running;
};

/**
 * @brief Set up multiple UDP sockets with different source ports
 * 
 * @param dest_ip Destination IP address
 * @param dest_port Destination port number
 * @param dest_addr Pointer to store the configured destination address
 * @param num_ports Number of source ports to create
 * @param sockets Array to store the created socket file descriptors
 * @return int 0 on success, -1 on failure
 */
static int setup_sockets(const char *dest_ip, int dest_port, struct sockaddr_in *dest_addr,
                        int num_ports, int *sockets) {
    // Set up destination address structure
    memset(dest_addr, 0, sizeof(*dest_addr));
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest_addr->sin_addr) <= 0) {
        perror("Invalid destination address");
        return -1;
    }

    // Create sockets with different source ports
    for (int i = 0; i < num_ports; i++) {
        // Create a UDP socket
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("socket creation failed");
            // Clean up previously created sockets
            for (int j = 0; j < i; j++) {
                close(sockets[j]);
            }
            return -1;
        }

        // Optimize socket for performance
        int optval = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        
        // Increase send buffer size for better throughput
        int sendbuf = 1024 * 1024;  // 1MB send buffer
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
        
        // Set non-blocking mode for better performance
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

        // Bind to a random source port
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        // Try to bind to a random port in the ephemeral port range
        int bound = 0;
        for (int attempts = 0; attempts < 10 && !bound; attempts++) {
            int src_port = MIN_SOURCE_PORT + (rand() % (MAX_SOURCE_PORT - MIN_SOURCE_PORT));
            src_addr.sin_port = htons(src_port);
            
            if (bind(sockfd, (struct sockaddr *)&src_addr, sizeof(src_addr)) == 0) {
                bound = 1;
                printf("Bound to source port %d\n", src_port);
            }
        }

        if (!bound) {
            perror("Failed to bind to any source port");
            close(sockfd);
            // Clean up previously created sockets
            for (int j = 0; j < i; j++) {
                close(sockets[j]);
            }
            return -1;
        }

        sockets[i] = sockfd;
    }

    return 0;
}

/**
 * @brief Initialize OpenSSL and create encryption context
 * 
 * @param ctx Pointer to store the created encryption context
 * @param key Pointer to store the encryption key
 * @return int 0 on success, -1 on failure
 */
static int init_openssl(EVP_CIPHER_CTX **ctx, unsigned char *key) {
    // Initialize OpenSSL
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();

    // Create and initialize the encryption context
    *ctx = EVP_CIPHER_CTX_new();
    if (!*ctx) {
        fprintf(stderr, "Failed to create cipher context\n");
        return -1;
    }

    // Create all-0 key
    memset(key, 0, AES_BLOCK_SIZE);
    return 0;
}

/**
 * @brief Generate a random IV
 * 
 * @param iv Buffer to store the generated IV
 * @return int 0 on success, -1 on failure
 */
static int generate_iv(unsigned char *iv) {
    if (RAND_bytes(iv, AES_IV_SIZE) != 1) {
        fprintf(stderr, "Failed to generate random IV\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Generate random plaintext data
 * 
 * @param data_size Size of data to generate (1 to MAX_DATA_SIZE)
 * @param plaintext Buffer to store the generated plaintext
 * @return int 0 on success, -1 on failure
 */
static int generate_plaintext(int data_size, unsigned char *plaintext) {
    if (data_size <= 0 || data_size > MAX_DATA_SIZE) {
        fprintf(stderr, "Invalid data size: %d (must be between 1 and %d)\n", data_size, MAX_DATA_SIZE);
        return -1;
    }

    if (RAND_bytes(plaintext, data_size) != 1) {
        fprintf(stderr, "Failed to generate random data\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Encrypt data using AES-GCM with optimized context reuse
 * 
 * @param ctx Encryption context (reused)
 * @param key Encryption key
 * @param iv Initialization vector
 * @param plaintext Data to encrypt
 * @param data_size Size of data to encrypt
 * @param ciphertext Buffer to store encrypted data
 * @param tag Buffer to store authentication tag
 * @return int 0 on success, -1 on failure
 */
static int encrypt_data(EVP_CIPHER_CTX *ctx, const unsigned char *key, const unsigned char *iv,
                       const unsigned char *plaintext, int data_size,
                       unsigned char *ciphertext, unsigned char *tag) {
    int len;

    // Reset context for reuse (more efficient than reinitializing)
    EVP_CIPHER_CTX_reset(ctx);
    
    // Initialize encryption
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, key, iv) != 1) {
        fprintf(stderr, "Failed to initialize encryption\n");
        return -1;
    }

    // Encrypt the data
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, data_size) != 1) {
        fprintf(stderr, "Encryption failed\n");
        return -1;
    }

    // Finalize encryption
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) {
        fprintf(stderr, "Encryption finalization failed\n");
        return -1;
    }

    // Get the authentication tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, AES_TAG_SIZE, tag) != 1) {
        fprintf(stderr, "Failed to get authentication tag\n");
        return -1;
    }

    return 0;
}

/**
 * @brief Generate a batch of packets
 * 
 * @param ctx Encryption context
 * @param key Encryption key
 * @param batch Array to store generated packets
 * @param batch_size Number of packets to generate
 * @param num_sockets Number of available sockets
 * @return int 0 on success, -1 on failure
 */
static int generate_packet_batch(EVP_CIPHER_CTX *ctx, const unsigned char *key,
                               struct packet_info *batch, int batch_size, int num_sockets) {
    for (int i = 0; i < batch_size; i++) {
        // Generate random IV
        unsigned char iv[AES_IV_SIZE];
        if (generate_iv(iv) != 0) {
            return -1;
        }

        // Choose random data size (1 to MAX_DATA_SIZE)
        int data_size = (rand() % MAX_DATA_SIZE) + 1;
        
        // Generate random plaintext
        unsigned char plaintext[MAX_DATA_SIZE];
        if (generate_plaintext(data_size, plaintext) != 0) {
            return -1;
        }

        // Encrypt the data
        unsigned char ciphertext[MAX_DATA_SIZE];
        unsigned char tag[AES_TAG_SIZE];
        if (encrypt_data(ctx, key, iv, plaintext, data_size, ciphertext, tag) != 0) {
            return -1;
        }

        // Construct the packet: IV + ciphertext + tag
        memcpy(batch[i].data, iv, AES_IV_SIZE);
        memcpy(batch[i].data + AES_IV_SIZE, ciphertext, data_size);
        memcpy(batch[i].data + AES_IV_SIZE + data_size, tag, AES_TAG_SIZE);
        
        batch[i].size = AES_IV_SIZE + data_size + AES_TAG_SIZE;
        batch[i].socket_index = rand() % num_sockets;
    }
    
    return 0;
}

/**
 * @brief Send a batch of packets using sendmmsg for maximum throughput
 * 
 * @param sockets Array of socket file descriptors
 * @param dest_addr Destination address
 * @param batch Array of packets to send
 * @param batch_size Number of packets in batch
 * @param show_stats_only Whether to show only statistics
 * @param dest_ip Destination IP for logging
 * @param dest_port Destination port for logging
 * @return int Number of packets successfully sent
 */
static int send_packet_batch(int *sockets, struct sockaddr_in *dest_addr,
                           struct packet_info *batch, int batch_size,
                           int show_stats_only, const char *dest_ip, int dest_port) {
    // Group packets by socket for batch sending
    struct mmsghdr *msgs = malloc(batch_size * sizeof(struct mmsghdr));
    struct iovec *iovecs = malloc(batch_size * sizeof(struct iovec));
    
    if (!msgs || !iovecs) {
        free(msgs);
        free(iovecs);
        return 0;
    }

    // Set up message headers for sendmmsg
    for (int i = 0; i < batch_size; i++) {
        iovecs[i].iov_base = batch[i].data;
        iovecs[i].iov_len = batch[i].size;
        
        msgs[i].msg_hdr.msg_name = dest_addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(*dest_addr);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_control = NULL;
        msgs[i].msg_hdr.msg_controllen = 0;
        msgs[i].msg_hdr.msg_flags = 0;
        msgs[i].msg_len = 0;
    }

    // Send all packets in a single system call
    int sent = sendmmsg(sockets[batch[0].socket_index], msgs, batch_size, 0);
    
    if (sent < 0) {
        perror("sendmmsg failed");
        sent = 0;
    }

    // Update statistics
    for (int i = 0; i < sent; i++) {
        if (msgs[i].msg_len > 0) {
            // Show per-packet output for low rates
            if (!show_stats_only) {
                struct sockaddr_in src_addr;
                socklen_t addr_len = sizeof(src_addr);
                getsockname(sockets[batch[i].socket_index], (struct sockaddr *)&src_addr, &addr_len);
                printf("Sent %u bytes from port %d to %s:%d\n",
                       msgs[i].msg_len, ntohs(src_addr.sin_port), dest_ip, dest_port);
            }
        }
    }

    free(msgs);
    free(iovecs);
    return sent;
}

/**
 * @brief Worker thread function for parallel packet generation and sending
 * 
 * @param arg Thread context
 * @return void* NULL
 */
static void* worker_thread(void *arg) {
    struct thread_context *ctx = (struct thread_context *)arg;
    
    // Pre-allocate batch buffer for this thread
    struct packet_info *batch = malloc(BATCH_SIZE * sizeof(struct packet_info));
    if (!batch) {
        return NULL;
    }

    while (ctx->running) {
        // Generate a batch of packets
        if (generate_packet_batch(ctx->ctx, ctx->key, batch, BATCH_SIZE, ctx->num_sockets) != 0) {
            continue;
        }

        // Send the batch
        int sent = send_packet_batch(ctx->sockets, ctx->dest_addr, batch, BATCH_SIZE, 
                                   ctx->show_stats_only, ctx->dest_ip, ctx->dest_port);
        
        if (sent > 0) {
            // Update statistics using atomic operations
            atomic_fetch_add(&ctx->stats->packets_sent, sent);
            atomic_fetch_add(&ctx->stats->packets_this_second, sent);
            
            // Calculate total bytes sent
            for (int i = 0; i < sent; i++) {
                atomic_fetch_add(&ctx->stats->bytes_sent, batch[i].size);
                atomic_fetch_add(&ctx->stats->bytes_this_second, batch[i].size);
            }
        }
    }

    free(batch);
    return NULL;
}

/**
 * @brief Clean up OpenSSL resources
 * 
 * @param ctx Encryption context to free
 */
static void cleanup_openssl(EVP_CIPHER_CTX *ctx) {
    if (ctx) {
        EVP_CIPHER_CTX_free(ctx);
    }
    EVP_cleanup();
    ERR_free_strings();
}

/**
 * @brief Print program usage information
 * 
 * @param program_name Name of the program
 */
static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [OPTIONS] <Destination IP>\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h, --help           Display this help message\n");
    fprintf(stderr, "  -p, --port PORT      Destination port (default: %d)\n", PARTIAL_DECRYPTION_TRAFFIC_PORT);
    fprintf(stderr, "  -r, --rate RATE      Sending rate in packets per second (default: %d)\n", DEFAULT_RATE);
    fprintf(stderr, "                       Use 0 for maximum rate (no sleep)\n");
    fprintf(stderr, "  -s, --sources NUM    Number of source ports to use (default: %d)\n", DEFAULT_SOURCE_PORTS);
    fprintf(stderr, "  -t, --threads NUM    Number of worker threads (default: %d, 1=single-threaded)\n", DEFAULT_THREADS);
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s -p 1234 -r 20 -s 5 127.0.0.1\n", program_name);
    fprintf(stderr, "  %s -r 0 -s 5 -t 4 127.0.0.1  # Multi-threaded maximum rate mode\n", program_name);
}

int main(int argc, char *argv[]) {
    const char *dest_ip = NULL;
    int dest_port = PARTIAL_DECRYPTION_TRAFFIC_PORT;
    int rate = DEFAULT_RATE;
    int num_source_ports = DEFAULT_SOURCE_PORTS;
    int num_threads = DEFAULT_THREADS;
    int opt;

    // Define long options
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"rate", required_argument, 0, 'r'},
        {"sources", required_argument, 0, 's'},
        {"threads", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };

    // Parse command line options
    while ((opt = getopt_long(argc, argv, "hp:r:s:t:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            case 'p':
                dest_port = atoi(optarg);
                if (dest_port <= 0 || dest_port > 65535) {
                    fprintf(stderr, "Invalid port number: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'r':
                rate = atoi(optarg);
                if (rate < 0) {
                    fprintf(stderr, "Invalid rate: %s (must be non-negative)\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 's':
                num_source_ports = atoi(optarg);
                if (num_source_ports <= 0) {
                    fprintf(stderr, "Invalid number of source ports: %s (must be positive)\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 't':
                num_threads = atoi(optarg);
                if (num_threads <= 0) {
                    fprintf(stderr, "Invalid number of threads: %s (must be positive)\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case '?':
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    // Check if destination IP is provided
    if (optind >= argc) {
        fprintf(stderr, "Error: Destination IP is required\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    dest_ip = argv[optind];

    // Initialize random number generator
    srand(time(NULL));

    // Set up sockets and destination address
    struct sockaddr_in dest_addr;
    int *sockets = malloc(num_source_ports * sizeof(int));
    if (!sockets) {
        fprintf(stderr, "Failed to allocate memory for sockets\n");
        return EXIT_FAILURE;
    }

    if (setup_sockets(dest_ip, dest_port, &dest_addr, num_source_ports, sockets) != 0) {
        free(sockets);
        return EXIT_FAILURE;
    }

    // Initialize OpenSSL
    EVP_CIPHER_CTX *ctx;
    unsigned char key[AES_BLOCK_SIZE];
    if (init_openssl(&ctx, key) != 0) {
        for (int i = 0; i < num_source_ports; i++) {
            close(sockets[i]);
        }
        free(sockets);
        return EXIT_FAILURE;
    }

    // Pre-allocate batch buffer
    struct packet_info *batch = malloc(BATCH_SIZE * sizeof(struct packet_info));
    if (!batch) {
        fprintf(stderr, "Failed to allocate memory for batch\n");
        cleanup_openssl(ctx);
        for (int i = 0; i < num_source_ports; i++) {
            close(sockets[i]);
        }
        free(sockets);
        return EXIT_FAILURE;
    }

    // Calculate sleep time in microseconds (0 if rate is 0)
    useconds_t sleep_time = rate == 0 ? 0 : 1000000 / rate;

    // Initialize statistics
    struct timeval start_time, current_time, last_stats_time;
    gettimeofday(&start_time, NULL);
    last_stats_time = start_time;
    unsigned long long total_packets = 0;
    unsigned long long total_bytes = 0;
    unsigned long long packets_this_second = 0;
    unsigned long long bytes_this_second = 0;
    int show_stats_only = rate == 0 || rate >= HIGH_RATE_THRESHOLD;

    printf("Starting %s packet generation:\n", num_threads == 1 ? "single-threaded" : "multi-threaded");
    printf("  Destination: %s:%d\n", dest_ip, dest_port);
    printf("  Rate: %s\n", rate == 0 ? "Maximum (no sleep)" : 
           rate >= HIGH_RATE_THRESHOLD ? "High (statistics only)" : 
           "Normal (per-packet output)");
    if (rate > 0) {
        printf("  Target rate: %d packets/second\n", rate);
        printf("  Sleep time: %u microseconds\n", sleep_time);
    }
    printf("  Source ports: %d\n", num_source_ports);
    printf("  Batch size: %d packets\n", BATCH_SIZE);
    printf("  Threads: %d\n", num_threads);

    if (num_threads == 1) {
        // Single-threaded mode
        while (1) {
            // Generate a batch of packets
            if (generate_packet_batch(ctx, key, batch, BATCH_SIZE, num_source_ports) != 0) {
                continue;
            }

            // Send the batch
            int sent = send_packet_batch(sockets, &dest_addr, batch, BATCH_SIZE, 
                                       show_stats_only, dest_ip, dest_port);
            
            if (sent > 0) {
                // Update statistics
                total_packets += sent;
                packets_this_second += sent;
                
                // Calculate total bytes sent
                for (int i = 0; i < sent; i++) {
                    total_bytes += batch[i].size;
                    bytes_this_second += batch[i].size;
                }

                // Get current time
                gettimeofday(&current_time, NULL);
                double elapsed = (current_time.tv_sec - last_stats_time.tv_sec) +
                               (current_time.tv_usec - last_stats_time.tv_usec) / 1000000.0;

                // Display statistics every second for high rates
                if (show_stats_only && elapsed >= STATS_INTERVAL) {
                    printf("Rate: %.1f packets/sec (%.1f MB/sec)\n",
                           packets_this_second / elapsed,
                           (bytes_this_second / elapsed) / (1024.0 * 1024.0));
                    
                    // Reset counters
                    packets_this_second = 0;
                    bytes_this_second = 0;
                    last_stats_time = current_time;
                }
            }

            // Sleep to maintain the desired rate (if not in maximum rate mode)
            if (rate > 0) {
                usleep(sleep_time);
            }
        }
    } else {
        // Multi-threaded mode
        pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
        struct thread_context *thread_ctxs = malloc(num_threads * sizeof(struct thread_context));
        
        if (!threads || !thread_ctxs) {
            fprintf(stderr, "Failed to allocate memory for threads\n");
            cleanup_openssl(ctx);
            for (int i = 0; i < num_source_ports; i++) {
                close(sockets[i]);
            }
            free(sockets);
            free(batch);
            free(threads);
            free(thread_ctxs);
            return EXIT_FAILURE;
        }
        
        for (int i = 0; i < num_threads; i++) {
            thread_ctxs[i].thread_id = i;
            thread_ctxs[i].sockets = sockets;
            thread_ctxs[i].num_sockets = num_source_ports;
            thread_ctxs[i].dest_addr = &dest_addr;
            thread_ctxs[i].dest_ip = dest_ip;
            thread_ctxs[i].dest_port = dest_port;
            thread_ctxs[i].show_stats_only = show_stats_only;
            thread_ctxs[i].ctx = EVP_CIPHER_CTX_new();  // Each thread gets its own context
            thread_ctxs[i].key = key;
            thread_ctxs[i].stats = malloc(sizeof(struct thread_stats));
            thread_ctxs[i].running = 1;
            
            // Initialize per-thread statistics
            if (thread_ctxs[i].stats) {
                atomic_init(&thread_ctxs[i].stats->packets_sent, 0);
                atomic_init(&thread_ctxs[i].stats->bytes_sent, 0);
                atomic_init(&thread_ctxs[i].stats->packets_this_second, 0);
                atomic_init(&thread_ctxs[i].stats->bytes_this_second, 0);
            }
            
            // Initialize encryption context for this thread
            EVP_CIPHER_CTX_reset(thread_ctxs[i].ctx);
            EVP_EncryptInit_ex(thread_ctxs[i].ctx, EVP_aes_128_gcm(), NULL, key, NULL);
            
            if (pthread_create(&threads[i], NULL, worker_thread, &thread_ctxs[i]) != 0) {
                fprintf(stderr, "Failed to create thread %d\n", i);
                // Clean up
                for (int j = 0; j < i; j++) {
                    thread_ctxs[j].running = 0;
                    pthread_join(threads[j], NULL);
                    EVP_CIPHER_CTX_free(thread_ctxs[j].ctx);
                    free(thread_ctxs[j].stats);
                }
                cleanup_openssl(ctx);
                for (int j = 0; j < num_source_ports; j++) {
                    close(sockets[j]);
                }
                free(sockets);
                free(batch);
                free(threads);
                free(thread_ctxs);
                return EXIT_FAILURE;
            }
        }

        // Main statistics display loop for multi-threaded mode
        while (1) {
            // Sleep to maintain the desired rate (if not in maximum rate mode)
            if (rate > 0) {
                usleep(sleep_time);
            }

            // Display statistics every second for high rates
            if (show_stats_only) {
                gettimeofday(&current_time, NULL);
                double elapsed = (current_time.tv_sec - last_stats_time.tv_sec) +
                               (current_time.tv_usec - last_stats_time.tv_usec) / 1000000.0;

                if (elapsed >= STATS_INTERVAL) {
                    // Collect statistics from all threads without locks
                    unsigned long long total_packets_this_second = 0;
                    unsigned long long total_bytes_this_second = 0;
                    
                    for (int i = 0; i < num_threads; i++) {
                        if (thread_ctxs[i].stats) {
                            total_packets_this_second += atomic_load(&thread_ctxs[i].stats->packets_this_second);
                            total_bytes_this_second += atomic_load(&thread_ctxs[i].stats->bytes_this_second);
                        }
                    }
                    
                    printf("Rate: %.1f packets/sec (%.1f MB/sec)\n",
                           total_packets_this_second / elapsed,
                           (total_bytes_this_second / elapsed) / (1024.0 * 1024.0));
                    
                    // Reset counters in all threads
                    for (int i = 0; i < num_threads; i++) {
                        if (thread_ctxs[i].stats) {
                            atomic_store(&thread_ctxs[i].stats->packets_this_second, 0);
                            atomic_store(&thread_ctxs[i].stats->bytes_this_second, 0);
                        }
                    }
                    
                    last_stats_time = current_time;
                }
            }
        }

        // Cleanup (this won't be reached in normal operation)
        for (int i = 0; i < num_threads; i++) {
            thread_ctxs[i].running = 0;
            pthread_join(threads[i], NULL);
            EVP_CIPHER_CTX_free(thread_ctxs[i].ctx);
            free(thread_ctxs[i].stats);
        }
        
        free(threads);
        free(thread_ctxs);
    }

    // Cleanup
    cleanup_openssl(ctx);
    for (int i = 0; i < num_source_ports; i++) {
        close(sockets[i]);
    }
    free(sockets);
    free(batch);
    return EXIT_SUCCESS;
} 
 