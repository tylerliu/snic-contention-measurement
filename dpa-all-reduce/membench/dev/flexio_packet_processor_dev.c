/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "com_dev.h"
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-libc/string.h>
#include <stddef.h>
#include <dpaintrin.h>
#include "../flexio_packet_processor_com.h"
#include "libflexio-dev/flexio_dev.h"

uint64_t bench_result[MAX_THREADS];
uint64_t bench_cycles[MAX_THREADS];

static uint64_t bench_mem_write(size_t *data, long num_ele, int stream_id, int stride, size_t loop, volatile uint64_t* progress, volatile uint64_t* stop, uint64_t* out_cycles) {
	size_t loop_num = 0;
	(void)stream_id;
	size_t start = __dpa_thread_cycles();
    
    // If loop is 0, we run until stop is set (continuous mode logic, but user might pass large loop + stop)
    // Actually, let's just use the loop count. If continuous, host passes huge loop count.
	while (loop_num < loop) {
        if (stop && *stop) break;

		for (long i = 0; i < num_ele; i += stride) {
			data[i] = loop_num;
		}
		loop_num++;
        
        if (progress) {
             *progress += (num_ele * sizeof(size_t));
             // Ensure visibility occasionally? 
             // __dpa_thread_fence(__DPA_SYSTEM, __DPA_W, __DPA_W); 
             // Doing fence every loop might be too expensive. Let's do it implicitly or relying on PCIe ordering.
             // But for safety, let's fence every 16 loops or so?
             if ((loop_num & 0xF) == 0) __dpa_thread_fence(__DPA_SYSTEM, __DPA_W, __DPA_W);
        }
	}
	size_t end = __dpa_thread_cycles();
	size_t total_cycles = (end - start);
    if (out_cycles) *out_cycles = total_cycles;
    
    // Bandwidth calculation (referenced from mt_memory)
    // Assuming 1800000000UL is CPU frequency or related constant
	return loop_num * num_ele * sizeof(size_t) * 1800000000UL / stride / total_cycles;
}

static uint64_t bench_mem_read(size_t *data, long num_ele, int stream_id, int stride, size_t loop, volatile uint64_t* progress, volatile uint64_t* stop, uint64_t* out_cycles) {
	size_t loop_num = 0;
    size_t sum[4] = {0};
	(void)stream_id;
	size_t start = __dpa_thread_cycles();
    
	while (loop_num < loop) {
        if (stop && *stop) break;

		for (long i = 0; i < num_ele; i += stride) {
			sum[0] += data[i];
		}
		loop_num++;
        
        if (progress) {
             *progress += (num_ele * sizeof(size_t));
             if ((loop_num & 0xF) == 0) __dpa_thread_fence(__DPA_SYSTEM, __DPA_W, __DPA_W);
        }
	}
	size_t end = __dpa_thread_cycles();
	size_t total_cycles = (end - start);
    if (out_cycles) *out_cycles = total_cycles;
    
    // Prevent optimization
    if (sum[0] == 0xdeadbeef) flexio_dev_print("Sum: %ld\n", sum[0]);

	return loop_num * num_ele * sizeof(size_t) * 1800000000UL / stride / total_cycles;
}

flexio_dev_rpc_handler_t flexio_pp_dev;
__dpa_rpc__ uint64_t flexio_pp_dev(uint64_t thread_arg_daddr)
{
    // Argument passed is a pointer to MemoryArg on DPA heap (copied from host)
	struct MemoryArg *arg = (struct MemoryArg *)thread_arg_daddr;
    // flexio_dev_print("DPA Entry: ArgPtr: 0x%lx\n", thread_arg_daddr);
    int stream_id = arg->stream_id;

    if (stream_id >= MAX_THREADS) return 0;

	// flexio_dev_print("Thread %d started. MemType: %d\n", stream_id, arg->mem_type);

	uint64_t *data = NULL;
	if (arg->mem_type == HOST_MEM) {
		data = (uint64_t *)get_host_buffer(arg->window_id, arg->mkey, (void*)arg->haddr);
	} else if (arg->mem_type == PRIVATE_MEM) {
		data = (uint64_t*)arg->ddata;
	}

    volatile uint64_t* progress = NULL;
    if (arg->progress_daddr) {
        progress = (uint64_t*)get_host_buffer(arg->window_id, arg->mkey, (void*)arg->progress_daddr);
    }
    
    volatile uint64_t* stop = NULL;
    if (arg->stop_daddr) {
        stop = (uint64_t*)get_host_buffer(arg->window_id, arg->mkey, (void*)arg->stop_daddr);
    }

    if (!data) {
        // flexio_dev_print("Thread %d: Failed to get data buffer\n", stream_id);
    } 
    
	long num_ele = arg->size / sizeof(size_t);
    uint64_t res = 0;
    uint64_t cycles = 0;
	if (arg->mem_type == HOST_MEM) {
        if (arg->op == OP_READ)
		    res = bench_mem_read((size_t*)data, num_ele, stream_id, arg->stride, arg->loop, progress, stop, &cycles);
        else 
		    res = bench_mem_write((size_t*)data, num_ele, stream_id, arg->stride, arg->loop, progress, stop, &cycles);
	} else {
        if (arg->op == OP_READ)
		    res = bench_mem_read((size_t*)data, num_ele, stream_id, arg->stride, arg->loop, progress, stop, &cycles);
        else 
		    res = bench_mem_write((size_t*)data, num_ele, stream_id, arg->stride, arg->loop, progress, stop, &cycles);
	}
    bench_result[stream_id] = res;
    bench_cycles[stream_id] = cycles;

    // Mark completion
    uint64_t* flag_addr = (uint64_t *)get_host_buffer(arg->window_id, arg->mkey, (void*)arg->completion_flag);
    if (flag_addr) {
        *flag_addr = 1;
        // Make sure it's visible?
          __dpa_thread_fence(__DPA_SYSTEM, __DPA_W, __DPA_W);
    } else {
         // flexio_dev_print("Thread %d: Failed to get completion flag buffer\n", stream_id);
    }
    
    return 0;
}

flexio_dev_rpc_handler_t get_job_result;
__dpa_rpc__ uint64_t get_job_result(uint64_t total_threads)
{
	uint64_t result = 0;
	for (size_t i = 0; i < total_threads; i++) {
		result += bench_result[i];
	}
	return result;
}

flexio_dev_rpc_handler_t get_job_cycles;
__dpa_rpc__ uint64_t get_job_cycles(uint64_t total_threads)
{
	uint64_t result = 0;
	for (size_t i = 0; i < total_threads; i++) {
		result += bench_cycles[i];
	}
	return result;
}

