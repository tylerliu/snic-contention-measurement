/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Define Version BEFORE includes */
#include <libflexio/flexio_ver.h>
#define FLEXIO_VER_USED FLEXIO_VER(25, 7, 0)

#include <cstdio>
#include <unistd.h>
#include <chrono>
#include <vector>
#include <string>
#include <thread>
#include <csignal>
#include <gflags/gflags.h>

#include <infiniband/mlx5dv.h>
#include <libflexio/flexio.h>

/* New shared header */
#include "../flexio_packet_processor_com.h"
/* Extracted memory classes */
#include "dpa_host_memory.h"

extern "C" {
extern flexio_func_t flexio_pp_dev;
extern flexio_func_t get_job_result;
extern flexio_func_t get_job_cycles;
}

/* CLI Flags */
DEFINE_string(device, "", "IB Device Name");
DEFINE_int32(threads, 1, "Number of threads");
DEFINE_int64(mem_size, 1024 * 1024 * 16, "Memory size (bytes)");
DEFINE_int64(loop, 100, "Loop count");
DEFINE_int32(stride, 1, "Stride");
DEFINE_bool(use_private, false, "Use private memory instead of host memory");
DEFINE_string(op, "write", "Operation: read, write");

DEFINE_bool(continuous, false, "Run in continuous mode");

volatile sig_atomic_t g_stop = 0;
void signal_handler(int signum) {
    g_stop = 1;
}

/* Command Queue Wrapper */
class CommandQueue {
public:
    struct flexio_cmdq *cmd_q = nullptr;
    struct flexio_process *process = nullptr;

    CommandQueue(struct flexio_process *proc, int num_threads, int batch_size) : process(proc) {
        flexio_cmdq_attr cmdq_attr = {0};
        cmdq_attr.workers = num_threads;
        cmdq_attr.batch_size = batch_size; // Usually 1 for independent tasks
        // cmdq_attr.state = FLEXIO_CMDQ_STATE_PENDING; // Default is pending

        if (flexio_cmdq_create(process, &cmdq_attr, &cmd_q) != FLEXIO_STATUS_SUCCESS) {
            throw std::runtime_error("Failed to create command queue");
        }
    }

    ~CommandQueue() {
        if (cmd_q) flexio_cmdq_destroy(cmd_q);
    }

    void add_task(flexio_func_t func, flexio_uintptr_t ddata) {
        if (flexio_cmdq_task_add(cmd_q, func, ddata) != FLEXIO_STATUS_SUCCESS) {
             throw std::runtime_error("Failed to add task to command queue");
        }
    }

    void run() {
        if (flexio_cmdq_state_running(cmd_q) != FLEXIO_STATUS_SUCCESS) {
             throw std::runtime_error("Failed to run command queue");
        }
    }
    
    bool is_empty() {
        return flexio_cmdq_is_empty(cmd_q);
    }

    /**
     * Wait until the command queue is empty.
     * 
     * @param timeout_sec Timeout in seconds
     * @return True if the command queue is empty, false otherwise
     */
    bool wait_until_empty(int timeout_sec) {
        auto start = std::chrono::high_resolution_clock::now();
        while (!flexio_cmdq_is_empty(cmd_q)) {
            auto now = std::chrono::high_resolution_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > timeout_sec) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

/* Application class to manage resources */
class App {
public:
    struct ibv_context *ctx = nullptr;
    struct ibv_pd *pd = nullptr;
    struct flexio_process *process = nullptr;
    struct flexio_window *window = nullptr;
    struct flexio_msg_stream *stream = nullptr;
    
    // Resources
    // std::vector<std::unique_ptr<DPAMemory>> dpa_args_mem; // Moved to local

    App(std::string dev_name) {
        open_device(dev_name);
        create_process();
    }

    ~App() {
        if (stream) flexio_msg_stream_destroy(stream);
        if (window) flexio_window_destroy(window);
        if (process) flexio_process_destroy(process);
        if (ctx) ibv_close_device(ctx);
    }

    void open_device(std::string dev_name) {
        int num_devices;
        struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
        if (!dev_list) throw std::runtime_error("Failed to get device list");

        for (int i = 0; i < num_devices; i++) {
            if (dev_name.empty() || dev_name == ibv_get_device_name(dev_list[i])) {
                ctx = ibv_open_device(dev_list[i]);
                break; 
            }
        }
        ibv_free_device_list(dev_list);
        if (!ctx) throw std::runtime_error("Failed to open device");
        
        pd = ibv_alloc_pd(ctx); // We need PD for HostMemory registration
        if (!pd) throw std::runtime_error("Failed to alloc PD");
    }

/* Helper macros for stringification */
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

    void create_process() {
        if (flexio_version_set(FLEXIO_VER_USED)) throw std::runtime_error("Failed to set version");

        struct flexio_app_select_attr app_selector = {0};
        app_selector.app_name = TOSTRING(DEV_APP_NAME);
        app_selector.ibv_ctx = ctx;

        struct flexio_app *app = nullptr;
        if (flexio_app_get(&app_selector, &app)) throw std::runtime_error("Failed to get app");

        if (flexio_process_create(ctx, app, nullptr, &process)) throw std::runtime_error("Failed to create process");
        
        if (flexio_window_create(process, pd, &window)) throw std::runtime_error("Failed to create window");
        
        // Setup stream
        struct flexio_msg_stream_attr stream_attr = {0};
        stream_attr.level = FLEXIO_MSG_DEV_INFO;
        stream_attr.data_bsize = 64 * 1024; // 64KB
        stream_attr.sync_mode = FLEXIO_MSG_DEV_SYNC_MODE_SYNC;
        stream_attr.transport_mode = FLEXIO_MSG_TRANSPORT_QP_RC;
        if (flexio_msg_stream_create(process, &stream_attr, stdout, nullptr, &stream) != FLEXIO_STATUS_SUCCESS) {
            throw std::runtime_error("Failed to create msg stream");
        }
    }
    
    void run_benchmark() {
        int threads = FLAGS_threads;
        size_t mem_size_val = FLAGS_mem_size;
        
        // Local resources to ensure destruction before process_destroy
        std::vector<std::unique_ptr<DPAMemory>> dpa_args_mem;
        std::unique_ptr<DPAMemory> private_buffer;

        int op_val = OP_WRITE;
        if (FLAGS_op == "read") op_val = OP_READ;
        else if (FLAGS_op == "write") op_val = OP_WRITE;
        else {
             printf("Invalid op: %s. Using write.\n", FLAGS_op.c_str());
        }

        // Layout in Unified Host Mem:
        // [Flags (threads * 64B)] [Progress (threads * 64B)] [StopFlag (64B)] [Data (if host) ...]
        
        size_t flags_size = 64 * threads * sizeof(uint64_t); // Completion flags (each flag is 64B aligned)
        size_t progress_offset = flags_size;
        size_t progress_size = 64 * threads * sizeof(uint64_t); // Progress counters (host order, each 64B aligned)
        size_t stop_offset = progress_offset + progress_size;
        size_t stop_size = 64; // One cacheline for stop flag
        size_t data_offset = stop_offset + stop_size;
        
        size_t total_host_size = data_offset + (FLAGS_use_private ? 0 : mem_size_val * threads);
        
        std::unique_ptr<HostMemory> unified_host_mem = std::make_unique<HostMemory>(pd, total_host_size, 4096);
        
        // Use the MR LKey directly for window access
        uint32_t mkey_id = unified_host_mem->mr->lkey;
        
        if (FLAGS_use_private) {
            private_buffer = std::make_unique<DPAMemory>(process, mem_size_val * threads);
        }

        uint64_t* flags_base = (uint64_t*)unified_host_mem->host_ptr;
        uint64_t* progress_base = (uint64_t*)((char*)unified_host_mem->host_ptr + progress_offset);
        uint64_t* stop_flag = (uint64_t*)((char*)unified_host_mem->host_ptr + stop_offset);
        void* data_base = (char*)unified_host_mem->host_ptr + data_offset;
        
        // Init
        memset(unified_host_mem->host_ptr, 0, total_host_size);

        // Create Command Queue
        CommandQueue cmdq(process, threads, 1);
        
        // Hook signal
        if (FLAGS_continuous) {
            signal(SIGINT, signal_handler);
            signal(SIGTERM, signal_handler);
        }

        for (int i = 0; i < threads; i++) {
            struct MemoryArg arg = {0};
            arg.stream_id = i;
            arg.window_id = flexio_window_get_id(window);
            arg.mkey = mkey_id;
            arg.mem_type = FLAGS_use_private ? PRIVATE_MEM : HOST_MEM;
            arg.op = op_val;
            arg.size = mem_size_val;
            arg.stride = FLAGS_stride;
            arg.loop = FLAGS_loop; // Use fixed loop for both modes (Host loops in continuous)
            
            // Completion Flag Address (Host Address)
            arg.completion_flag = (flexio_uintptr_t)((char*)flags_base + i * 64 * sizeof(uint64_t)); // Stride flags
            
            // Progress/Stop for continuous mode
            if (FLAGS_continuous) {
                 arg.progress_daddr = (flexio_uintptr_t)((char*)progress_base + i * 64 * sizeof(uint64_t));
                 arg.stop_daddr = (flexio_uintptr_t)stop_flag;
            }

            if (FLAGS_use_private) {
                arg.ddata = private_buffer->daddr + i * mem_size_val;
            } else {
                arg.haddr = (flexio_uintptr_t)((char*)data_base + i * mem_size_val);
            }
            
            // Copy Arg to DPA
            auto dpa_arg = std::make_unique<DPAMemory>(process, sizeof(MemoryArg));
            if (flexio_host2dev_memcpy(process, &arg, sizeof(MemoryArg), dpa_arg->daddr) != FLEXIO_STATUS_SUCCESS) {
                 throw std::runtime_error("Arg Copy failed");
            }
            
            // Store to keep alive
            dpa_args_mem.push_back(std::move(dpa_arg));
        }
        
        printf("Starting Command Queue with %d threads (Continuous: %s)...\n", threads, FLAGS_continuous ? "YES" : "NO");
        
        

        // Prime the pump: Start first batch
        for (int i = 0; i < threads; i++) {
            cmdq.add_task(flexio_pp_dev, dpa_args_mem[i]->daddr);
        }
        cmdq.run();
        auto start = std::chrono::high_resolution_clock::now();
        
        while (!g_stop) {
            // Wait for current batch to complete
            while (!cmdq.is_empty()) {
                if (g_stop) break;
                if (cmdq.wait_until_empty(1)) {
                    break;
                }
            }
            if (g_stop) break;
            
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = end-start;
            
            // Now Process Results for the batch that just finished
            printf("Finished. Time: %.2fs\n", diff.count());
            
            uint64_t total_tp = 0;
            uint64_t rpc_ret = 0;

            flexio_process_call(process, &get_job_result, &rpc_ret, threads);
            total_tp = rpc_ret;

            uint64_t total_cycles = 0;
            flexio_process_call(process, &get_job_cycles, &total_cycles, threads);
            double avg_cycles = (double)total_cycles / threads;

            if (!FLAGS_continuous) {
                printf("Total ");
            }
            printf("Throughput (Device Reported): %.2f MB/s (Avg Time: %.2f s)\n", (double)total_tp / 1024.0 / 1024.0, avg_cycles / 1800000000UL);
            
            if (!FLAGS_continuous) {
                 break; 
            }
            // Collect the completed batch before its results can be overwritten.
            for (int i = 0; i < threads; i++) {
                cmdq.add_task(flexio_pp_dev, dpa_args_mem[i]->daddr);
            }
            cmdq.run();
            start = std::chrono::high_resolution_clock::now();
        }
        
    }
};

int main(int argc, char** argv) {
    gflags::SetUsageMessage("Usage: flexio_packet_processor [options]\n"
                            "Options:\n"
                            "  --threads <n>       Number of threads (max 256)\n"
                            "  --mem_size <bytes>  Memory size per thread (bytes)\n"
                            "  --stride <n>        Stride for memory access\n"
                            "  --loop <n>          Number of loops\n"
                            "  --use_private       Use DPA private memory (default: false)\n"
                            "  --op <read|write>   Operation type (default: write)\n"
                            "  --device <name>     IB Device Name (optional)\n");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    setbuf(stdout, NULL);
    
    try {
        App app(FLAGS_device);
        
        // Test DPA Communication
        // uint64_t ret_val;
        // if (flexio_process_call(app.process, &hello_dpa, &ret_val, 0) != FLEXIO_STATUS_SUCCESS) {
        //     printf("Warning: Hello RPC failed\n");
        // } 

        app.run_benchmark();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}
