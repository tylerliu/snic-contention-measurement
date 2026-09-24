#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <set>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <doca_log.h>
#include <infiniband/verbs.h>
#include <thread>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include "common/ring_buffer.h"
#include "nicfs/stages.h"

#include "common/dpdk_init.h"
#include "common/dpdk_offloads.h"
#include "file_system/xts_log_processor.h"
#include "file_system/gcm_log_processor.h"
#include "common/doca_dma.h"
#include "common/verbs_helper.h"
#include "common/tcp_helper.h"
#include "common/doorbell_channel.h"
#include "common/disk_ring.h"
#include "common/pause_tracker.h"
// Config
constexpr size_t kChunkSize = 256 * 1024; // 256KiB batch size
constexpr size_t kRemoteRegionSize = 2 * kLogAreaSize; // 2 * 8MB

#ifndef LITE_DMA_DEFAULT_ALIGN
#define LITE_DMA_DEFAULT_ALIGN 64
#endif

struct BootstrapDescs {
	std::string log_desc;
	std::string disk_desc;
};

struct ReplicaGids {
	union ibv_gid primary_gid;  // GID of primary NIC (for ACKs)
	union ibv_gid upstream_gid; // GID of upstream replica (for chain replication)
};

static void accept_connections(TcpServer& server,
                              BootstrapDescs& descs,
                              std::unique_ptr<DmaUser>& dma_user,
                              std::unique_ptr<DmaUser>& dma_user_disk,
                              VerbsManager& verbs_manager,
                              DoorbellListenerChannel& chan,
                              VerbsManager& verbs_manager_persist,
                              DoorbellRingerChannel& persist_ringer,
                              std::vector<std::unique_ptr<VerbsManager>>& replica_verbs_managers,
                              std::vector<std::unique_ptr<DoorbellRingerChannel>>& replica_send_channels,
                              std::vector<DoorbellRingerChannel*>& replica_ack_channels,
                              const std::vector<std::string>& replica_identities,
                              const std::string& pci_addr) {
	bool need_hostfs = true;
	std::set<std::string> remaining_replicas(replica_identities.begin(), replica_identities.end());
	int hostfs_sock = -1;
	std::vector<int> replica_socks(replica_identities.size(), -1);
	
	while (need_hostfs || !remaining_replicas.empty()) {
		std::string id;
		int sock = server.Accept(id);
		if (id == "hostfs" && need_hostfs) {
			// Handle hostfs bootstrap + RDMA handshake in one connection
			std::vector<TcpKvItem> items;
			tcp_read_kv_batch(sock, items);
			for (const auto& it : items) {
				if (it.key == "LOG") {
					descs.log_desc.assign(reinterpret_cast<const char*>(it.value.data()), it.value.size());
				} else if (it.key == "DISK") {
					descs.disk_desc.assign(reinterpret_cast<const char*>(it.value.data()), it.value.size());
				}
			}
            // RDMA exchange via K/V on the same socket: read client's RDMA_LOCAL, reply with ours
			verbs_manager.PrepareLocalConnData();
            verbs_manager_persist.PrepareLocalConnData();
			server.ingest_kv_batch_from_socket(sock, id);
			{
				const auto& kvmap = server.get_kv_map(id);
				auto itRdma = kvmap.find("RDMA_LOCAL");
				if (itRdma != kvmap.end()) {
					RdmaConnData peer = {};
					std::memcpy(&peer, itRdma->second.data(), sizeof(RdmaConnData));
					verbs_manager.SetRemoteConnData(peer);
					verbs_manager.ConnectToRemote();
				}
				auto itRdma2 = kvmap.find("RDMA_LOCAL_PERSIST");
                if (itRdma2 != kvmap.end()) {
                    RdmaConnData peer = {};
                    std::memcpy(&peer, itRdma2->second.data(), sizeof(RdmaConnData));
                    verbs_manager_persist.SetRemoteConnData(peer);
                    verbs_manager_persist.ConnectToRemote();
                }
				std::vector<TcpKvItem> reply;
				reply.push_back(TcpKvItem{std::string("RDMA_LOCAL"),
					std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&verbs_manager.GetLocalConnData()),
										 reinterpret_cast<const uint8_t*>(&verbs_manager.GetLocalConnData()) + sizeof(RdmaConnData))});                std::vector<TcpKvItem> reply2;
				reply.push_back(TcpKvItem{std::string("RDMA_LOCAL_PERSIST"),
					std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&verbs_manager_persist.GetLocalConnData()),
										reinterpret_cast<const uint8_t*>(&verbs_manager_persist.GetLocalConnData()) + sizeof(RdmaConnData))});
				server.send_kv_batch_to_identity(id, reply);
			}
			// Register buffers after handshake (no extra TCP identity)
			chan.setup_after_handshake();
            persist_ringer.setup_after_handshake();
			
			dma_user = std::unique_ptr<DmaUser>(new DmaUser(pci_addr, descs.log_desc, kRemoteRegionSize));
            if (!descs.disk_desc.empty()) {
                dma_user_disk = std::unique_ptr<DmaUser>(new DmaUser(pci_addr, descs.disk_desc, kRemoteRegionSize));
            }
			std::cout << "[nicfs] Hostfs bootstrap and RDMA doorbell channel ready." << std::endl;
			need_hostfs = false;
			hostfs_sock = sock; // keep open for KV GID distribution
		} else if (remaining_replicas.find(id) != remaining_replicas.end()) {
			// Find which replica this is in the chain
			size_t replica_idx = 0;
			for (size_t i = 0; i < replica_identities.size(); ++i) {
				if (replica_identities[i] == id) {
					replica_idx = i;
					break;
				}
			}
			
			// RDMA exchange on this socket via K/V: read client's RDMA_LOCAL, reply with ours
			replica_verbs_managers[replica_idx]->PrepareLocalConnData();
			server.ingest_kv_batch_from_socket(sock, id);
			const auto& kvmapR = server.get_kv_map(id);
			{
				auto itRdmaR = kvmapR.find("RDMA_LOCAL");
				if (itRdmaR != kvmapR.end()) {
					RdmaConnData peer = {};
					std::memcpy(&peer, itRdmaR->second.data(), sizeof(RdmaConnData));
					replica_verbs_managers[replica_idx]->SetRemoteConnData(peer);
					replica_verbs_managers[replica_idx]->ConnectToRemote();
				}
				std::vector<TcpKvItem> reply;
				reply.push_back(TcpKvItem{std::string("RDMA_LOCAL"),
					std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&replica_verbs_managers[replica_idx]->GetLocalConnData()),
										 reinterpret_cast<const uint8_t*>(&replica_verbs_managers[replica_idx]->GetLocalConnData()) + sizeof(RdmaConnData))});
				server.send_kv_batch_to_identity(id, reply);
			}
			{
				// Read tester region size to set remote region bounds
				auto itSize = kvmapR.find("RECEIVER_REGION_SIZE");
				if (itSize != kvmapR.end() && itSize->second.size() == sizeof(size_t)) {
					size_t region_sz = *reinterpret_cast<const size_t*>(itSize->second.data());
					std::cout << "[nicfs] Receiver region size received: " << region_sz << " bytes" << std::endl;
					if (region_sz < kRemoteRegionSize) {
						throw std::runtime_error("Replica remote region too small");
					}
				} else {
					throw std::runtime_error("Replica " + id + " did not send RECEIVER_REGION_SIZE");
				}
			}
			// Register buffers after handshake (no extra TCP identity)
			replica_send_channels[replica_idx]->setup_after_handshake();
			
			// Test doorbell/ACK mechanism before starting data flow
			std::cout << "[nicfs] Testing doorbell/ACK mechanism with replica " << id << "..." << std::endl;
			replica_send_channels[replica_idx]->send_doorbell(0, 0, 1024);
			std::cout << "[nicfs] Sent test doorbell to replica " << id << ", waiting for ACK..." << std::endl;
			
			// Wait for ACK
			AckMsg ack;
			bool received = false;
			std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();
			while (std::chrono::high_resolution_clock::now() - start_time < std::chrono::seconds(1)) {
				if (replica_send_channels[replica_idx]->poll_ack(ack)) {
					std::cout << "[nicfs] Received ACK for seq=" << ack.seq << " from replica " << id << std::endl;
					received = true;
					break;
				} else {
					rte_pause(); // busy-wait
				}
			}
			if (!received) {
				std::cout << "[nicfs] ERROR: No ACK received for test doorbell from replica " << id << "!" << std::endl;
				exit(1);
			}

            // Keep socket open for later GID KV distribution
			replica_socks[replica_idx] = sock;
			replica_ack_channels.push_back(replica_send_channels[replica_idx].get());
			
			std::cout << "[nicfs] Replica " << id << " (send+ack) channel ready." << std::endl;
			remaining_replicas.erase(id);
		} else {
			std::cout << "[nicfs] Unknown identity: " << id << ", closing connection" << std::endl;
			close(sock);
		}
	}

	// After all nodes are connected, distribute GIDs via K/V to each sock
	union ibv_gid primary_gid = verbs_manager.GetLocalConnData().gid;
    std::cout << "[nicfs] Remote MR addr/rkey for replication target(s):" << std::endl;
    for (size_t i = 0; i < replica_verbs_managers.size(); ++i) {
        const auto &r = replica_verbs_managers[i]->GetRemoteConnData();
        std::cout << "  replica[" << i << "] mr_addr=" << (void*)r.mr_addr << " rkey=" << r.mr_rkey << std::endl;
    }
	// Prepare quick lambda to serialize a gid
	auto gid_to_vec = [](const union ibv_gid& gid) -> std::vector<uint8_t> {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(&gid);
		return std::vector<uint8_t>(p, p + sizeof(union ibv_gid));
	};

	// HostFS: send PRIMARY_GID
	if (hostfs_sock >= 0) {
		std::vector<TcpKvItem> kvout;
		kvout.push_back(TcpKvItem{std::string("PRIMARY_GID"), gid_to_vec(primary_gid)});
		server.send_kv_batch_to_identity("hostfs", kvout);
		server.close_client("hostfs");
	}

	// Replicas: send PRIMARY_GID, UPSTREAM_GID, DOWNSTREAM_GID
	for (size_t i = 0; i < replica_socks.size(); ++i) {
		int sock = replica_socks[i];
		if (sock < 0) continue;
		union ibv_gid upstream_gid;
		union ibv_gid downstream_gid;
		if (i == 0) {
			upstream_gid = primary_gid;
		} else {
			upstream_gid = replica_verbs_managers[i - 1]->GetRemoteConnData().gid;
		}
		if (i + 1 < replica_socks.size()) {
			downstream_gid = replica_verbs_managers[i + 1]->GetRemoteConnData().gid;
		} else {
			downstream_gid = primary_gid;
		}
			std::vector<TcpKvItem> kvout;
			kvout.push_back(TcpKvItem{std::string("PRIMARY_GID"), gid_to_vec(primary_gid)});
			kvout.push_back(TcpKvItem{std::string("UPSTREAM_GID"), gid_to_vec(upstream_gid)});
			kvout.push_back(TcpKvItem{std::string("DOWNSTREAM_GID"), gid_to_vec(downstream_gid)});
			server.send_kv_batch_to_identity(replica_identities[i], kvout);
			server.close_client(replica_identities[i]);
	}
}

// removed single-threaded async loop in favor of per-stage threads


static void set_thread_affinity(std::thread &thr, int cpu_index) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<unsigned>(cpu_index), &cpuset);
	int rc = pthread_setaffinity_np(thr.native_handle(), sizeof(cpu_set_t), &cpuset);
	if (rc != 0) {
		std::cerr << "[nicfs] Warning: pthread_setaffinity_np failed for cpu " << cpu_index << " rc=" << rc << std::endl;
	}
}

static std::set<int> parse_core_map(const std::vector<std::string> &core_map) {
	std::set<int> cores;
	if (core_map.empty()) return cores;
	for (const auto& core : core_map) {
		if (core.find('-') != std::string::npos) {
			int start, end;
			sscanf(core.c_str(), "%d-%d", &start, &end);
			for (int i = start; i <= end; i++) {
				cores.insert(i);
			}
		} else {
			cores.insert(std::stoi(core));
		}
	}
	return cores;
}

static void run_nicfs_threaded(DoorbellListenerChannel& chan,
								 DmaUser& log_dma,
								 DmaUser& disk_dma,
								 std::unique_ptr<VerbsManager>* replica_verbs,
								 std::unique_ptr<DoorbellRingerChannel>* replica_send,
								 std::vector<DoorbellRingerChannel*>& replica_ack_channels,
								 DoorbellRingerChannel& persist_ringer,
							 const std::set<int>& core_map,
							 bool track_pauses,
							 const std::string& log_encryption) {
	std::unique_ptr<LogProcessor> log_processor_ptr;
	std::unique_ptr<CryptoDevice> crypto_dev; // Base class for XTS/GCM
	
	if (log_encryption == "aes-xts") {
		// Initialize crypto device 0 with multiple queue pairs for parallel processing
		uint16_t num_workers = 6; 
		crypto_dev.reset(new AesXtsDevice(0, num_workers, CRYPTO_DEFAULT_NB_DESCRIPTORS));
		log_processor_ptr.reset(new XtsLogProcessor(static_cast<AesXtsDevice*>(crypto_dev.get()), num_workers));
	} else if (log_encryption == "aes-gcm") {
		uint16_t num_workers = 6; 
		crypto_dev.reset(new AesGcmDevice(0, num_workers, CRYPTO_DEFAULT_NB_DESCRIPTORS));
		log_processor_ptr.reset(new GcmLogProcessor(static_cast<AesGcmDevice*>(crypto_dev.get()), num_workers));
	} else {
		log_processor_ptr.reset(new MemcpyLogProcessor());
	}
	constexpr size_t ring_capacity = kLogAreaSize / kChunkSize * 2;
	SpscRing<ChunkMsg> ring12(ring_capacity);
	SpscRing<BatchMsg> ring12meta(ring_capacity);
	SpscRing<ChunkMsg> ring23(ring_capacity);
	SpscRing<ChunkMsg> ring24(ring_capacity);
	SpscRing<BatchMsg> ringPersistDone(ring_capacity);
	SpscRing<BatchMsg> ringRepDone(ring_capacity);
	SpscRing<uint64_t> ring21freeing(ring_capacity);
	SpscRing<BatchMsg> procBufferFreeingRing(ring_capacity);

	auto shared_allocator = disk_dma.get_local_region_ptr();

	ReceiverStage s1(chan, log_dma, ring12, ring12meta, ring21freeing, kLogAreaSize, kChunkSize);
	LogProcessorStage s2(*log_processor_ptr, ring12, ring12meta, ring21freeing, ring23, ring24, procBufferFreeingRing, shared_allocator);
	PersistenceStage s_persist(disk_dma, ring23, ringPersistDone, DISK_RING_REGION_SIZE, kChunkSize, persist_ringer);
	size_t remote_region_sz = replica_verbs ? kRemoteRegionSize : 0;
	ReplicationStage s_rep(replica_verbs ? replica_verbs->get() : nullptr, replica_send ? replica_send->get() : nullptr, replica_ack_channels, ring24, ringRepDone, remote_region_sz, kChunkSize, shared_allocator);
	AckCleanupStage s4(chan, procBufferFreeingRing, ringPersistDone, ringRepDone, shared_allocator);

	std::vector<std::thread> threads;
	std::unique_ptr<NullPauseTracker> s1_tracker(track_pauses ? new PauseTracker("nicfs-s1-receiver") : new NullPauseTracker());
	std::unique_ptr<NullPauseTracker> s2_tracker(track_pauses ? new PauseTracker("nicfs-s2-log-processor") : new NullPauseTracker());
	std::unique_ptr<NullPauseTracker> s_persist_tracker(track_pauses ? new PauseTracker("nicfs-s-persist") : new NullPauseTracker());
	std::unique_ptr<NullPauseTracker> s_rep_tracker(track_pauses ? new PauseTracker("nicfs-s-rep") : new NullPauseTracker());
	std::unique_ptr<NullPauseTracker> s4_tracker(track_pauses ? new PauseTracker("nicfs-s4-ack-cleanup") : new NullPauseTracker());
	threads.emplace_back([&]() {
		for (;;) { if (!s1.tick()) s1_tracker->pause(); }
	});
	threads.emplace_back([&]() {
		for (;;) { if (!s2.tick()) s2_tracker->pause(); }
	});
	threads.emplace_back([&]() {
		for (;;) { if (!s_persist.tick()) s_persist_tracker->pause(); }
	});
	threads.emplace_back([&]() {
		for (;;) { if (!s_rep.tick()) s_rep_tracker->pause(); }
	});
	threads.emplace_back([&]() {
		for (;;) { if (!s4.tick()) s4_tracker->pause(); }
	});
	if (log_processor_ptr->get_num_workers() > 1) {
		for (uint16_t i = 1; i < log_processor_ptr->get_num_workers(); i++) {
			threads.emplace_back([&, i]() {
				for (;;) { 
					bool progress_made = log_processor_ptr->process_worker_queue(i);
					progress_made |= log_processor_ptr->worker_poll(i);
					if (!progress_made) rte_pause();
				}
			});
		}
	}

	// Simple core mapping: assign threads to cores [0..N-1] in order
	unsigned int core_count = std::thread::hardware_concurrency();
	if (core_count == 0) core_count = 1;
	auto core_it = core_map.begin();
	for (auto &t : threads) {
		if (core_it != core_map.end()) {
			int core = *core_it++;
			set_thread_affinity(t, core);
			std::cout << "[nicfs] Pinned stage thread " << t.get_id() << " to CPU " << core << std::endl;
		} else {
			std::cout << "[nicfs] No more cores to pin, leaving thread " << t.get_id() << " unpinned" << std::endl;
		}
	}

	for (auto &t : threads) t.join();
}


int main(int argc, char *argv[]) {
	try {
		// 1. Initialize logging, DPDK and DOCA
		struct doca_log_backend *sdk_log = NULL;
		(void)doca_log_backend_create_standard();
		if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) == DOCA_SUCCESS) {
			(void)doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
		}

		AppLifecycleManager::init("LiteFS NICFS");
		AppLifecycleManager& app = AppLifecycleManager::get_instance();
		app.set_use_argp_dpdk_program(true);
		auto &parser = app.parser();
		parser.add_option("pci").alias("p")
		      .help("PCI address of the DOCA device (optional)")
		      .default_value("");
		parser.add_option("ibv-device").alias("b")
		      .help("RDMA verbs device name (e.g., mlx5_0)")
		      .default_value("");
		parser.add_option("listen-port").alias("L")
		      .help("Port to listen on for host connection")
		      .default_value("8080");
		parser.add_option("replicas").alias("r")
		      .argument_count('*')
		      .help("list of replica endpoints host:port");
		parser.add_option("cores").alias("C")
			  .argument_count('*')
		      .help("list of CPU cores for stage threads");
		parser.add_option("track-pauses").alias("t")
		      .help("Enable pause tracking for performance monitoring")
		      .argument_count('0');
		parser.add_option("log-encryption").alias("E")
		      .help("Log encryption mode: aes-xts, aes-gcm, or none")
		      .default_value("none");
		app.parse(argc, argv);
		std::cout << "[nicfs] Args: pci='" << app.get_string("pci")
		          << "' ibv-device='" << app.get_string("ibv-device")
		          << "' listen-port=" << app.get_int("listen-port")
		          << " replicas='" << app.get_string("replicas")
		          << "' track-pauses=" << (app.get_flag("track-pauses") ? "true" : "false") << std::endl;

		// RDMA managers: one for host doorbells, one for persistence doorbells, multiple for replica chain
		VerbsManager verbs_manager(app.get_string("ibv-device"), 1, 0);
		VerbsManager verbs_manager_persist(app.get_string("ibv-device"), 1, 0);
		std::vector<std::unique_ptr<VerbsManager>> replica_verbs_managers;
		std::vector<std::unique_ptr<DoorbellRingerChannel>> replica_send_channels;
		std::vector<DoorbellRingerChannel*> replica_ack_channels;

		std::cout << "[nicfs] Core components initialized." << std::endl;

		// 2. Prepare RDMA doorbell channels and handshake (log + persist)
		DoorbellListenerChannel chan(verbs_manager);
		DoorbellRingerChannel persist_ringer(verbs_manager_persist);

		// 3. Act as central server for bootstrap and unordered handshakes
		TcpServer server(static_cast<uint16_t>(app.get_int("listen-port")));
		BootstrapDescs descs;

        // Objects for DMA path
        std::unique_ptr<DmaUser> dma_user;       // for LOG (pull from remote)
        std::unique_ptr<DmaUser> dma_user_disk;  // for DISK ring writes

		// Parse replicas as identity names (comma-separated chain)
		std::vector<std::string> replica_identities = app.context().values("replicas");
		
		// Create verbs managers and channels for each replica
		replica_verbs_managers.reserve(replica_identities.size());
		replica_send_channels.reserve(replica_identities.size());
		for (size_t i = 0; i < replica_identities.size(); ++i) {
			replica_verbs_managers.emplace_back(new VerbsManager(app.get_string("ibv-device"), 1, 0));
			replica_send_channels.emplace_back(new DoorbellRingerChannel(*replica_verbs_managers.back()));
		}
		
        // Accept connections in any order: hostfs (bootstrap+rdma), replicas (identities from arg)
        accept_connections(server, descs, dma_user, dma_user_disk, verbs_manager, chan, verbs_manager_persist, persist_ringer, replica_verbs_managers, replica_send_channels, replica_ack_channels, replica_identities, app.get_string("pci"));

		// 5. RDMA doorbell loop: async processing with optional replication
		std::cout << "[nicfs] Ready to receive RDMA doorbells (async)." << std::endl;
		auto core_map = parse_core_map(app.context().values("cores"));
		bool track_pauses = app.get_flag("track-pauses");
		std::string log_encryption = app.get_string("log-encryption");
		// Backward compatibility for xts-log flag if we wanted keep it, but we are replacing it. 
		// If user passes old flag, it might error if strict, but we removed it.
		
		run_nicfs_threaded(chan, *dma_user, *dma_user_disk, replica_verbs_managers.empty() ? nullptr : &replica_verbs_managers[0], replica_send_channels.empty() ? nullptr : &replica_send_channels[0], replica_ack_channels, persist_ringer, core_map, track_pauses, log_encryption);
	} catch (const std::exception& e) {
		std::cerr << "[nicfs] Caught exception: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "[nicfs] Test finished." << std::endl;
	return 0;
}
