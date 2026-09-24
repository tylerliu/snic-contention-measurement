#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <doca_log.h>
#include <deque>
#include <queue>
#include <thread>

#include "common/dpdk_init.h"
#include "common/doca_dma.h"
#include "common/tcp_helper.h"
#include "common/verbs_helper.h"
#include "common/doorbell_channel.h"
#include "file_system/formatter.h"
#include "file_system/log_gen.h"
#include "file_system/operator.h"
#include "common/disk_ring.h"
#include "common/pause_tracker.h"

constexpr size_t minBatchSize = 256 * 1024;
constexpr size_t maxBatchSize = 256 * 1024 * 32;        // 256KiB doorbell unit

DOCA_LOG_REGISTER(LITEFS_HOSTFS);

static void run_hostfs_async_loop(void* log_area, size_t ring_size, DoorbellRingerChannel& chan, bool track_pauses) {
    LogGenerator generator;
    size_t start = 0; // first unprocessed byte (consumer head)
    size_t end = 0;   // producer tail
    size_t cut = 0;   // reserved for future multi-region batching
    struct InflightBatch { uint64_t seq; size_t offset; size_t length; };
    std::deque<InflightBatch> inflight;
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> acked_seqs;
    srand(time(nullptr));

    std::unique_ptr<NullPauseTracker> async_tracker(track_pauses ? new PauseTracker("hostfs-async-loop") : new NullPauseTracker());

    for (uint32_t seq = 0; seq < UINT32_MAX; ) {
        // 1) Dequeue all available ACKs (may arrive out of order)
        AckMsg ack = {};
        while (chan.poll_ack(ack)) {
            if (ack.magic == LITEFS_ACK_MAGIC) {
                acked_seqs.emplace(ack.seq);
            }
        }

        // 2) Retire in-flight batches strictly in order
        while (!inflight.empty() && !acked_seqs.empty() && acked_seqs.top() == inflight.front().seq) {
            const auto retired = inflight.front();
            inflight.pop_front();
            acked_seqs.pop();
            assert(retired.offset == start);
            start += retired.length;
            if (end < start && start == cut) { start = 0; cut = 0; }
            DOCA_LOG_DBG("Retired seq=%ld len=%zu start(now)=%zu inflight=%zu", retired.seq, retired.length, start, inflight.size());
        }

        // 3) Decide if we can generate a new batch
        if (start == end) { // reset buffer pointer if it happens to be empty. 
            start = 0;
            end = 0;
        }
        size_t free_space = (start <= end) ? (ring_size - end) : (start - end - 1);
        if ((start > end || start == 0) && free_space < minBatchSize) { 
            // start > end means we have wrapped around, start == 0 mean we can't get new space by wrapping around
            // No room right now; backoff briefly and continue polling acks
            async_tracker->pause();
            continue;
        }

        size_t target_batch_size = minBatchSize + (rand() % (maxBatchSize - minBatchSize));
        target_batch_size = std::min(free_space, target_batch_size);

        // 4) Generate logs
        size_t batch_size = 0;
        const size_t batch_start_offset = end;
        if (seq == 0) {
            auto start_size = generator.generate_create_phase(static_cast<uint8_t*>(log_area) + end, target_batch_size);
            auto second_size = generator.generate_rewrite_phase(static_cast<uint8_t*>(log_area) + end + start_size, target_batch_size - start_size);
            batch_size = start_size + second_size;
        } else {
            batch_size = generator.generate_rewrite_phase(static_cast<uint8_t*>(log_area) + end, target_batch_size);
        }

        // 5) Advance producer tail with wrap handling
        size_t new_end = end + batch_size;
        if (end + target_batch_size == ring_size && start != 0) {
            cut = new_end;
            end = 0;
        } else {
            end = new_end;
        }

        // 6) Doorbell without blocking; track in-flight
        if (batch_size > 0) {
            seq++;
            DOCA_LOG_DBG("Doorbell seq=%d batch=%zu start_off=%zu inflight(before)=%zu", seq, batch_size, batch_start_offset, inflight.size());
            chan.send_doorbell(seq, batch_start_offset, batch_size, true);
            inflight.push_back(InflightBatch{seq, batch_start_offset, batch_size});
        } else {
            if (!(end == 0 && start > 0)) {
                // only when we wrap around, we can send a batch of 0
                DOCA_LOG_DBG("No batch to send at seq=%d", seq);
            }
        }
    }
}


int main(int argc, char *argv[]) {
    try {
        // Set up DOCA logging (match master style)
        struct doca_log_backend *sdk_log = NULL;
        (void)doca_log_backend_create_standard();
        if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) == DOCA_SUCCESS) {
            (void)doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
        }
        // 1. Initialize CLI and DPDK (match master argument names)
        AppLifecycleManager::init("LiteFS HostFS");
        AppLifecycleManager& app = AppLifecycleManager::get_instance();
        auto &parser = app.parser();
        parser.add_option("pci").alias("p")
              .help("PCI address of the DOCA device (optional)")
              .default_value("");
        parser.add_option("ibv-device").alias("b")
              .help("RDMA verbs device name (not used here)")
              .default_value("");
        parser.add_option("nic-ip").alias("i")
              .help("IP address of the nicfs application")
              .default_value("127.0.0.1");
        parser.add_option("nic-port").alias("n")
              .help("Port of the nicfs application")
              .default_value("8080");
        parser.add_option("track-pauses").alias("t")
              .help("Enable pause tracking for performance monitoring")
              .argument_count('0');
        app.parse(argc, argv);
        DOCA_LOG_INFO("[hostfs] Args: pci=%s ibv-device=%s nic-ip=%s nic-port=%lld track-pauses=%s", app.get_string("pci").c_str(), app.get_string("ibv-device").c_str(), app.get_string("nic-ip").c_str(), app.get_int("nic-port"), app.get_flag("track-pauses") ? "true" : "false");

        // 2. Allocate Log Area and Disk Area

        void *log_area = nullptr;
        void *disk_area = nullptr;
        if (posix_memalign(&log_area, 64, kLogAreaSize) != 0 || log_area == nullptr) {
            throw std::runtime_error("Failed to allocate Log Area");
        }
        if (posix_memalign(&disk_area, 64, kDiskAreaSize) != 0 || disk_area == nullptr) {
            throw std::runtime_error("Failed to allocate Disk Area");
        }
        DOCA_LOG_INFO("[hostfs] Allocated Log Area (%zu) and Disk Area (%zu)", kLogAreaSize, kDiskAreaSize);

        // 3. Initialize Disk Ring Header and format FS region at offset
        void* fs_base = static_cast<uint8_t*>(disk_area) + DISK_FS_BASE_OFFSET;

        FileSystemFormatter formatter;
        if (!formatter.format(fs_base, kDiskAreaSize - DISK_FS_BASE_OFFSET)) {
            throw std::runtime_error("Filesystem format failed");
        }
        DOCA_LOG_INFO("[hostfs] Disk Area formatted as LiteFS.");

        // 4. Export both regions
        DmaExporter log_exporter(app.get_string("pci"), static_cast<const char*>(log_area), kLogAreaSize);
        DmaExporter disk_exporter(app.get_string("pci"), static_cast<const char*>(disk_area), kDiskAreaSize);
        const std::string &log_desc = log_exporter.get_export_desc();
        const std::string &disk_desc = disk_exporter.get_export_desc();
        DOCA_LOG_INFO("[hostfs] Exported Log desc size=%zu, Disk desc size=%zu", log_desc.size(), disk_desc.size());

		// 5. Connect to nicfs, send descriptors, then do RDMA handshake on the SAME socket
		DOCA_LOG_INFO("[hostfs] Connecting to nicfs server at %s:%lld", app.get_string("nic-ip").c_str(), app.get_int("nic-port"));
		TcpClient client;
		client.connect(app.get_string("nic-ip"), static_cast<uint16_t>(app.get_int("nic-port")), "hostfs");
		DOCA_LOG_INFO("[hostfs] Connected. Sending LOG and DISK descriptors.");
		std::vector<TcpKvItem> items;
		items.push_back(TcpKvItem{std::string("LOG"), std::vector<uint8_t>(log_desc.begin(), log_desc.end())});
		items.push_back(TcpKvItem{std::string("DISK"), std::vector<uint8_t>(disk_desc.begin(), disk_desc.end())});
		client.send_kv_batch(items);
        // RDMA: prepare local, send our RDMA_LOCAL via K/V, then read server RDMA_LOCAL and connect
        VerbsManager verbs(app.get_string("ibv-device"), 1, 0);
        verbs.PrepareLocalConnData();
        // Second RDMA QP for persistence doorbells
        VerbsManager verbs_persist(app.get_string("ibv-device"), 1, 0);
        verbs_persist.PrepareLocalConnData();
        {
            TcpKvItem rdmaItem;
            rdmaItem.key = "RDMA_LOCAL";
            rdmaItem.value.assign(reinterpret_cast<const uint8_t*>(&verbs.GetLocalConnData()), reinterpret_cast<const uint8_t*>(&verbs.GetLocalConnData()) + sizeof(RdmaConnData));
            TcpKvItem rdmaItem2;
            rdmaItem2.key = "RDMA_LOCAL_PERSIST";
            rdmaItem2.value.assign(reinterpret_cast<const uint8_t*>(&verbs_persist.GetLocalConnData()), reinterpret_cast<const uint8_t*>(&verbs_persist.GetLocalConnData()) + sizeof(RdmaConnData));
            client.send_kv_batch(std::vector<TcpKvItem>{rdmaItem, rdmaItem2});
            client.read_kv_batch();
            std::vector<uint8_t> remote, remote2;
            if (client.get_kv("RDMA_LOCAL", remote)) {
                RdmaConnData peer = {};
                std::memcpy(&peer, remote.data(), sizeof(RdmaConnData));
                verbs.SetRemoteConnData(peer);
                verbs.ConnectToRemote();
            }
            if (client.get_kv("RDMA_LOCAL_PERSIST", remote2)) {
                RdmaConnData peer2 = {};
                std::memcpy(&peer2, remote2.data(), sizeof(RdmaConnData));
                verbs_persist.SetRemoteConnData(peer2);
                verbs_persist.ConnectToRemote();
            }
        }

        // Close the connection; further K/V (e.g., GIDs) are not required by hostfs
        client.close();
        DOCA_LOG_INFO("[hostfs] Descriptors sent and RDMA handshakes complete (log + persist).");

        // 6. Register buffers for doorbells/acks after handshake
        DoorbellRingerChannel chan(verbs);
        chan.setup_after_handshake();
        DOCA_LOG_INFO("[hostfs] RDMA doorbell channel ready (log sender).");

        // Persistence doorbell listener channel
        DoorbellListenerChannel persist_listener(verbs_persist);
        persist_listener.setup_after_handshake();
        DOCA_LOG_INFO("[hostfs] RDMA persistence doorbell listener ready.");

        // 7. Start consumer thread to digest logs using persistence doorbells and apply locally
        FileSystemOperator local_fs_op(static_cast<uint8_t*>(fs_base), kDiskAreaSize - DISK_FS_BASE_OFFSET);
        bool track_pauses = app.get_flag("track-pauses");
        std::unique_ptr<NullPauseTracker> consumer_tracker(track_pauses ? new PauseTracker("hostfs-consumer") : new NullPauseTracker());
        std::thread consumer([&]() {
            uint8_t* disk_bytes = static_cast<uint8_t*>(disk_area);
            uint64_t current_seq = 0;
            uint8_t* current_start = nullptr;
            size_t current_length = 0;
            size_t total_consumed = 0;
            while (true) {
                DoorbellMsg db = {};
                if (persist_listener.poll(db)) {
                    DOCA_LOG_DBG("hostfs: Received persist doorbell seq=%lu start=%lu len=%lu end_segment=%u", db.seq, db.start, db.length, db.end_segment);
                    if (current_seq != db.seq) {
                        assert(current_length == 0);
                        current_seq = db.seq;
                        current_length = 0;
                        current_start = disk_bytes + db.start;
                    }
                    assert(current_start + current_length == disk_bytes + db.start);
                    current_length += db.length;
                    DOCA_LOG_DBG("hostfs: current_start=%p total_consumed=%zu current_length=%zu", current_start, total_consumed, current_length);
                    size_t consumed = local_fs_op.process_logs(current_start + total_consumed, current_length - total_consumed);
                    DOCA_LOG_DBG("hostfs: consumed=%zu", consumed);
                    total_consumed += consumed;
                    if (db.end_segment) {
                        assert(total_consumed == current_length);
                        persist_listener.ack(db.seq, current_length);
                        current_seq = 0;
                        current_length = 0;
                        current_start = nullptr;
                        total_consumed = 0;
                        DOCA_LOG_DBG("hostfs: ACKed persist seq=%lu len=%lu", db.seq, (unsigned long)current_length);
                    }
                } else {
                    consumer_tracker->pause();
                }
            }
        });

        // 8. Generate logs and doorbell NIC when threshold reached (async, multiple in-flight)
        run_hostfs_async_loop(log_area, kLogAreaSize, chan, track_pauses);

        consumer.detach();

        // Clean up
        free(log_area);
        free(disk_area);
    } catch (const std::exception& e) {
        std::cerr << "[hostfs] Caught exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

