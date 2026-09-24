#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <infiniband/verbs.h>

#include "common/dpdk_init.h"
#include "common/verbs_helper.h"
#include "common/tcp_helper.h"
#include "common/doorbell.h"
#include "common/doorbell_channel.h"
#include "common/disk_ring.h"
#include "common/pause_tracker.h"

// Config
constexpr size_t kDataBufferSize = kLogAreaSize * 2; // 2x log area size to accommodate large writes

DOCA_LOG_REGISTER(LITEFS_NIC_TESTER);

// Statistics tracking structure
struct NicTesterStats {
    uint64_t total_sequences = 0;
    uint64_t total_segments = 0;
    uint64_t total_bytes = 0;
    std::chrono::high_resolution_clock::time_point last_print_time;
    
    NicTesterStats() : last_print_time(std::chrono::high_resolution_clock::now()) {}
    
    void record_sequence(uint64_t bytes) {
        total_sequences++;
        total_bytes += bytes;
    }
    
    void record_segment(uint64_t bytes) {
        total_segments++;
        total_bytes += bytes;
    }
    
    void print_and_reset_stats() {
        auto now = std::chrono::high_resolution_clock::now();
        auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_print_time).count();
        
        if (interval >= 1000000000LL) { // Print every second (1 billion nanoseconds)
            double interval_sec = interval / 1000000000.0;
            
            // Calculate rates for this interval
            double bytes_per_sec = total_bytes / interval_sec;
            double mb_per_sec = bytes_per_sec / (1024 * 1024); // Convert to MB/s
            
            std::cout << "[nic_tester] STATS: seqs=" << total_sequences 
                      << " segments=" << total_segments 
                      << " bytes=" << total_bytes 
                      << " rate=" << std::fixed << std::setprecision(2) << bytes_per_sec << " B/s"
                      << " (" << mb_per_sec << " MB/s)" << std::endl;
            
            // Reset counters for next interval
            total_sequences = 0;
            total_segments = 0;
            total_bytes = 0;
            last_print_time = now;
        }
    }
};

int main(int argc, char *argv[]) {
    try {
		// 1. Initialize logging, DPDK and DOCA
		struct doca_log_backend *sdk_log = NULL;
		(void)doca_log_backend_create_standard();
		if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) == DOCA_SUCCESS) {
			(void)doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
		}

        // 2. Initialize CLI and DPDK for RDMA tester
        AppLifecycleManager::init("LiteFS NIC RDMA Tester");
        AppLifecycleManager& app = AppLifecycleManager::get_instance();
        auto &parser = app.parser();
        parser.add_option("primary-ip").alias("s")
              .help("Server IP of nicfs")
              .default_value("127.0.0.1");
        parser.add_option("primary-tcp-port").alias("t")
               .help("TCP port of nicfs")
               .default_value("8080");
        parser.add_option("ibv-device").alias("b")
               .help("RDMA verbs device name (optional)")
               .default_value("");
        parser.add_option("track-pauses").alias("p")
               .help("Enable pause tracking for performance monitoring")
               .argument_count('0');
        app.parse(argc, argv);
        std::cout << "[nic_tester] Args: primary-ip=" << app.get_string("primary-ip")
                  << " primary-tcp-port=" << app.get_int("primary-tcp-port")
                  << " ibv-device='" << app.get_string("ibv-device")
                  << "' track-pauses=" << (app.get_flag("track-pauses") ? "true" : "false") << std::endl;

        // 3. Initialize RDMA
        VerbsManager verbs_manager(app.get_string("ibv-device"), 1, 0);
        std::cout << "[nic_tester] VerbsManager initialized." << std::endl;

        // 4. Allocate and register a large target buffer for RDMA writes and a small ACK buffer
        void* target_buf = nullptr;
        if (posix_memalign(&target_buf, 64, kDataBufferSize) != 0 || target_buf == nullptr) {
            throw std::runtime_error("Failed to allocate target_buf");
        }
        auto* target_mr = verbs_manager.RegisterMemory(target_buf, kDataBufferSize);

        // 5. Share target MR with server and post RECVs for ACKs from server
        verbs_manager.SetLocalMr((uint64_t)target_mr->addr, target_mr->rkey);
        std::cout << "[nic_tester] Registered target buffer (MR addr=" << (void*)target_mr->addr
                  << ", rkey=" << target_mr->rkey << ")" << std::endl;

        // 6. Connect to server and perform RDMA handshake
        std::cout << "[nic_tester] Connecting to nicfs server at " << app.get_string("primary-ip")
                  << ":" << app.get_int("primary-tcp-port") << std::endl;
        TcpClient client;
        client.connect(app.get_string("primary-ip"), static_cast<uint16_t>(app.get_int("primary-tcp-port")), "nic_tester");
        std::cout << "[nic_tester] Connected. Performing RDMA setup via K/V." << std::endl;
        verbs_manager.PrepareLocalConnData();
        TcpKvItem rdmaItem{std::string("RDMA_LOCAL"),
            std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&verbs_manager.GetLocalConnData()),
                                 reinterpret_cast<const uint8_t*>(&verbs_manager.GetLocalConnData()) + sizeof(RdmaConnData))};
        TcpKvItem sizeItem{ std::string("RECEIVER_REGION_SIZE"), std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&kDataBufferSize), 
                                 reinterpret_cast<const uint8_t*>(&kDataBufferSize) + sizeof(size_t))};
        client.send_kv_batch(std::vector<TcpKvItem>{rdmaItem, sizeItem});
        client.read_kv_batch();
        std::vector<uint8_t> remote;
        if (client.get_kv("RDMA_LOCAL", remote)) {
            RdmaConnData peer = {};
            std::memcpy(&peer, remote.data(), sizeof(RdmaConnData));
            verbs_manager.SetRemoteConnData(peer);
            verbs_manager.ConnectToRemote();
            std::cout << "[nic_tester] RDMA connected. Local QP="
                      << verbs_manager.GetLocalConnData().qp_num
                      << " Remote QP=" << verbs_manager.GetRemoteConnData().qp_num << std::endl;
        }
        client.close();
        std::cout << "[nic_tester] RDMA handshake complete. Waiting for doorbells and data..." << std::endl;

        // 7. Setup doorbell listener channel to receive doorbells from nicfs
        DoorbellListenerChannel doorbell_listener(verbs_manager);
        doorbell_listener.setup_after_handshake();
        std::cout << "[nic_tester] Doorbell listener channel ready." << std::endl;

        // 9. Test doorbell/ACK mechanism
        std::cout << "[nic_tester] Waiting for test doorbell from nicfs..." << std::endl;
        DoorbellMsg doorbell;
        bool received = false;
        std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();
        while (std::chrono::high_resolution_clock::now() - start_time < std::chrono::seconds(1)) {
            if (doorbell_listener.poll(doorbell)) {
                std::cout << "[nic_tester] Received test doorbell: seq=" << doorbell.seq 
                          << " start=" << doorbell.start << " length=" << doorbell.length << std::endl;
                // Send ACK back
                doorbell_listener.ack(doorbell.seq);
                std::cout << "[nic_tester] Sent ACK for seq=" << doorbell.seq << std::endl;
                received = true;
                break;
            } else {
                rte_pause(); // busy-wait
            }
        }
        if (!received) {
            std::cout << "[nic_tester] ERROR: No test doorbell received!" << std::endl;
            return 1;
        } else {
            std::cout << "[nic_tester] Doorbell/ACK test successful!" << std::endl;
        }

        // 10. Main loop: poll for doorbells and send ACKs
        // Note: The doorbell_listener.poll() method now properly handles CQE errors
        // by re-posting receive buffers to prevent exhaustion
        uint64_t current_unit_size = 0;
        uint64_t current_seq = 0;
        NicTesterStats stats;
        bool track_pauses = app.get_flag("track-pauses");
        std::unique_ptr<NullPauseTracker> main_tracker(track_pauses ? new PauseTracker("nic-tester-main") : new NullPauseTracker());
        
        std::cout << "[nic_tester] Starting main loop with statistics tracking..." << std::endl;
        
        while (true) {
            if (doorbell_listener.poll(doorbell)) {
                DOCA_LOG_DBG("Received doorbell: seq=%lu start=%lu length=%lu end_segment=%s", 
                             doorbell.seq, doorbell.start, doorbell.length, doorbell.end_segment ? "true" : "false");
                
                // Record segment statistics
                stats.record_segment(doorbell.length);
                
                // Check if this is a new unit (different sequence number)
                if (current_seq != doorbell.seq) {
                    // If we had a previous unit, it should have been completed
                    if (current_seq != 0) {
                        DOCA_LOG_ERR("Previous unit (seq=%lu) was not completed properly!", current_seq);
                        return 1;
                    }
                    // Start tracking new unit
                    current_seq = doorbell.seq;
                    current_unit_size = doorbell.length;
                } else {
                    // Same unit, accumulate the length
                    current_unit_size += doorbell.length;
                }
                
                // Only send ACK when this is the last segment of the unit
                if (doorbell.end_segment) {
                    DOCA_LOG_DBG("Last segment received for unit seq=%lu, total size=%lu bytes", doorbell.seq, current_unit_size);
                    
                    // Record sequence completion statistics
                    stats.record_sequence(current_unit_size);
                    
                    // Send ACK back with total unit size
                    doorbell_listener.ack(doorbell.seq, current_unit_size);
                    DOCA_LOG_DBG("Sent ACK for seq=%lu with total size=%lu bytes", doorbell.seq, current_unit_size);
                    
                    // Reset for next unit
                    current_seq = 0;
                    current_unit_size = 0;
                }
            } else {
                main_tracker->pause();
            }
            
            // Print statistics every second
            stats.print_and_reset_stats();
        }

    } catch (const std::exception& e) {
        std::cerr << "[nic_tester] Caught exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[nic_tester] Test finished." << std::endl;
    return 0;
}

