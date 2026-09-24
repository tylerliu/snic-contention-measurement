/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* Source file for device part of packet processing sample.
 * Contain functions for initialize contexts of internal queues,
 * read, check, change and resend the packet and wait for another.
 */

/* Shared header file with utilities for samples.
 * The file also have includes to flexio_dev_ver.h and flexio_dev.h
 * The include must be placed first to correctly handle the version.
 */
#include "com_dev.h"
#include <libflexio-dev/flexio_dev_err.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-libc/string.h>
#include <stddef.h>
#include <dpaintrin.h>
/* Shared header file for packet processor sample */
#include "../flexio_packet_processor_com.h"
#include "libflexio-dev/flexio_dev.h"

/* Mask for CQ index */
#define CQ_IDX_MASK ((1 << LOG_CQ_DEPTH) - 1)
/* Mask for RQ index */
#define RQ_IDX_MASK ((1 << LOG_RQ_DEPTH) - 1)
/* Mask for SQ index */
#define SQ_IDX_MASK ((1 << (LOG_SQ_DEPTH + LOG_SQE_NUM_SEGS)) - 1)
/* Mask for data index */
#define DATA_IDX_MASK ((1 << (LOG_SQ_DEPTH)) - 1)

/* The structure of the sample DPA application contains global data that the application uses */
/* The structure of the sample DPA application contains global data that the application uses */
struct thread_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;

	uint32_t rq_key;

	cq_ctx_t rq_cq_ctx;     /* RQ CQ */
	rq_ctx_t rq_ctx;        /* RQ */

	uint8_t device_mac[6];  /* Device Source MAC */
	uint8_t verbose;        /* Verbose mode */
	uint8_t rq_on_host;     /* RQ ring on host */
	host_rq_ctx_t host_rq_ctx; /* Host RQ context */
	uint64_t bytes_count; /* Local bytes count */
} __attribute__((__aligned__(64)));

/* Global array of thread contexts */
struct thread_context __thread_contexts [MAX_THREADS] __attribute__((__aligned__(64)));

/* Initialize the app_ctx structure from the host data.
 *  data_from_host - pointer host2dev_packet_processor_data from host.
 */
static void app_ctx_init(struct host2dev_packet_processor_data *data_from_host, struct thread_context *app_ctx)
{
	app_ctx->packets_count = 0;

	app_ctx->rq_key = data_from_host->rq_transf.wqd_mkey_id;
	/* Copy device MAC */
	memcpy(app_ctx->device_mac, data_from_host->device_mac, 6);
	app_ctx->verbose = data_from_host->verbose;
	app_ctx->rq_on_host = data_from_host->rq_on_host;
	if (app_ctx->rq_on_host) { /* Here app_ctx is a pointer to local struct */
		app_ctx->host_rq_ctx.rkey = data_from_host->rq_transf.wqd_mkey_id;
		app_ctx->host_rq_ctx.rq_window_id = data_from_host->rq_window_id;
		/* Initialize host_rx_buff with the host address passed from host side. */
		app_ctx->host_rq_ctx.host_rx_buff = (void*)data_from_host->rq_transf.wqd_daddr;
		/* Also set dpa_rx_buff to null initially or whatever is safe, but it will be set by get_host_buffer later */
		app_ctx->host_rq_ctx.dpa_rx_buff = 0; 
	}

	app_ctx->bytes_count = 0;

	/* Set context for RQ's CQ */
	com_cq_ctx_init(&app_ctx->rq_cq_ctx,
			data_from_host->rq_cq_transf.cq_num,
			data_from_host->rq_cq_transf.log_cq_depth,
			data_from_host->rq_cq_transf.cq_ring_daddr,
			data_from_host->rq_cq_transf.cq_dbr_daddr);

	/* Set context for RQ */
	com_rq_ctx_init(&app_ctx->rq_ctx,
			data_from_host->rq_transf.wq_num,
			data_from_host->rq_transf.wq_ring_daddr,
			data_from_host->rq_transf.wq_dbr_daddr);
}

/* process packet - just count it and release buffer */
static void process_packet(struct thread_context *app_ctx)
{
	/* Size of the data */
	uint32_t data_sz;

	/* Extract relevant data from the CQE */
	data_sz = flexio_dev_cqe_get_byte_cnt(app_ctx->rq_cq_ctx.cqe);
	app_ctx->bytes_count += data_sz;

	/* Ring DB */
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_dbr_rq_inc_pi(app_ctx->rq_ctx.rq_dbr);
}

/* Entry point function that host side call for the execute.
 *  thread_arg - pointer to the host2dev_packet_processor_data structure
 *     to transfer data from the host side.
 */
flexio_dev_event_handler_t flexio_pp_dev;
__dpa_global__ void flexio_pp_dev(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data *data_from_host = (void *)thread_arg;
	struct thread_context *app_ctx;

	/* Get thread context */
	if (data_from_host->thread_id >= MAX_THREADS) {
		/* Should not happen if host checks, but safety first */
		return;
	}
	app_ctx = &__thread_contexts[data_from_host->thread_id];

	/* If the thread is executed for first time, then initialize the context
	 */
	if (!data_from_host->not_first_run) {
		app_ctx_init(data_from_host, app_ctx);
		data_from_host->not_first_run = 1;
	}

	if (app_ctx->rq_on_host) { /* Now app_ctx is a pointer */
		app_ctx->host_rq_ctx.dpa_rx_buff = get_host_buffer(app_ctx->host_rq_ctx.rq_window_id, app_ctx->host_rq_ctx.rkey, app_ctx->host_rq_ctx.host_rx_buff);
	}

	/* Poll CQ until the package is received.
	 */
	int processed_packets = 0;
	while (flexio_dev_cqe_get_owner(app_ctx->rq_cq_ctx.cqe) !=
	       app_ctx->rq_cq_ctx.cq_hw_owner_bit && processed_packets < 10000) {
		/* Increment packet count */
		app_ctx->packets_count++;
		processed_packets++;
		
		/* Print the message */
		if (app_ctx->verbose)
			flexio_dev_print("Process packet: %ld\n", app_ctx->packets_count);
			
		/* Update memory to DPA */
		__dpa_thread_fence(__DPA_MEMORY, __DPA_R, __DPA_R);
		/* Process the packet */
		process_packet(app_ctx);
		/* Update RQ CQ */
		com_step_cq(&app_ctx->rq_cq_ctx);
	}
	/* Update the memory to the chip */
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	/* Arming cq for next packet */
	flexio_dev_cq_arm(app_ctx->rq_cq_ctx.cq_idx, app_ctx->rq_cq_ctx.cq_number);

	/* Reschedule the thread */
	flexio_dev_thread_reschedule();
}

flexio_dev_rpc_handler_t dpa_get_total_packets;
__dpa_rpc__ uint64_t dpa_get_total_packets(uint64_t dummy_arg)
{
	(void)dummy_arg;
	uint64_t total = 0;
	// Loop over all threads to sum packets
	// Note: MAX_THREADS is defined. We should really sum up to actual num_threads, but we don't have it global?
	// The array is MAX_THREADS sized. Unused threads should have 0 packets.
	for (int i = 0; i < MAX_THREADS; i++) {
		total += __thread_contexts[i].packets_count;
	}
	return total;
}

flexio_dev_rpc_handler_t dpa_get_total_bytes;
__dpa_rpc__ uint64_t dpa_get_total_bytes(uint64_t dummy_arg)
{
	(void)dummy_arg;
	uint64_t total = 0;
	for (int i = 0; i < MAX_THREADS; i++) {
		total += __thread_contexts[i].bytes_count;
	}
	return total;
}
