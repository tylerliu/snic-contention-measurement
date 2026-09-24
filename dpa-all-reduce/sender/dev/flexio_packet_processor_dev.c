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
#include "libflexio-dev/flexio_dev_queue_types.h"

/* Mask for CQ index */
#define CQ_IDX_MASK ((1 << LOG_CQ_DEPTH) - 1)
/* Mask for RQ index */
#define RQ_IDX_MASK ((1 << LOG_RQ_DEPTH) - 1)
/* Mask for SQ index */
#define SQ_IDX_MASK ((1 << (LOG_SQ_DEPTH + LOG_SQE_NUM_SEGS)) - 1)
/* Mask for data index */
#define DATA_IDX_MASK ((1 << (LOG_SQ_DEPTH)) - 1)

/* SQ batch size for flow control and CQE requests */
#define SQ_BATCH_SIZE 16

/* The structure of the sample DPA application contains global data that the application uses */
/* The structure of the sample DPA application contains global data that the application uses */
struct thread_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	/* lkey - local memory key */
	uint32_t lkey;
	uint32_t rq_key;

	cq_ctx_t rq_cq_ctx;     /* RQ CQ */
	rq_ctx_t rq_ctx;        /* RQ */
	sq_ctx_t sq_ctx;        /* SQ */
	cq_ctx_t sq_cq_ctx;     /* SQ CQ */
	dt_ctx_t dt_ctx;        /* SQ Data ring */

	uint8_t device_mac[6];  /* Device Source MAC */
	uint8_t verbose;        /* Verbose mode */
	uint8_t sq_on_host;     /* SQ data on host */
	host_rq_ctx_t host_rq_ctx; /* Host RQ context (can be repurposed or cleaned up) */
	uint64_t bytes_count; /* Local bytes count */
    uint32_t payload_size; /* Payload size */
    uint32_t sq_wqe_counter; /* Last completed SQ WQE index */
    uint32_t shared_sq_data_count; /* Total shared packets */
} __attribute__((__aligned__(64)));

/* Global array of thread contexts */
struct thread_context __thread_contexts [MAX_THREADS] __attribute__((__aligned__(64)));

/* Initialize the app_ctx structure from the host data.
 *  data_from_host - pointer host2dev_packet_processor_data from host.
 */
static void app_ctx_init_context(struct host2dev_packet_processor_data *data_from_host, struct thread_context *app_ctx)
{
	app_ctx->packets_count = 0;
	app_ctx->lkey = data_from_host->sq_transf.wqd_mkey_id;
	app_ctx->rq_key = data_from_host->rq_transf.wqd_mkey_id;
	/* Copy device MAC */
	memcpy(app_ctx->device_mac, data_from_host->device_mac, 6);
	app_ctx->verbose = data_from_host->verbose;
	app_ctx->sq_on_host = data_from_host->sq_on_host;
	if (app_ctx->sq_on_host) { 
        /* Repurpose host_rq_ctx for SQ data on host if needed, 
         * or just use the window ID from data_from_host later. */
	}



	app_ctx->bytes_count = 0;
    app_ctx->payload_size = data_from_host->payload_size;
    app_ctx->shared_sq_data_count = data_from_host->shared_sq_data_count;

    /* RQ Init Skipped for Sender */
    /*
	com_cq_ctx_init(&app_ctx->rq_cq_ctx, ...);
	com_rq_ctx_init(&app_ctx->rq_ctx, ...);
    */

	/* Set context for SQ */
	com_sq_ctx_init(&app_ctx->sq_ctx,
			data_from_host->sq_transf.wq_num,
			data_from_host->sq_transf.wq_ring_daddr);

	/* Set context for SQ's CQ */
	com_cq_ctx_init(&app_ctx->sq_cq_ctx,
			data_from_host->sq_cq_transf.cq_num,
			data_from_host->sq_cq_transf.log_cq_depth,
			data_from_host->sq_cq_transf.cq_ring_daddr,
			data_from_host->sq_cq_transf.cq_dbr_daddr);

	/* Set context for data */
	com_dt_ctx_init(&app_ctx->dt_ctx, data_from_host->sq_transf.wqd_daddr);
}

static void app_ctx_init_buffers(struct host2dev_packet_processor_data *data_from_host, struct thread_context *app_ctx)
{
    flexio_uintptr_t dpa_tx_buff_base = (flexio_uintptr_t)app_ctx->dt_ctx.sq_tx_buff;
    if (app_ctx->sq_on_host) {
        dpa_tx_buff_base = get_host_buffer(data_from_host->sq_window_id, app_ctx->lkey, (void*)dpa_tx_buff_base);
    }

    /* Initialization partitioning: each thread initializes a unique slice of the shared pool */
    uint32_t packets_per_thread = (app_ctx->shared_sq_data_count + data_from_host->num_threads - 1) / data_from_host->num_threads;
    uint32_t start_idx = data_from_host->thread_id * packets_per_thread;
    uint32_t end_idx = start_idx + packets_per_thread;
    if (end_idx > app_ctx->shared_sq_data_count) end_idx = app_ctx->shared_sq_data_count;

    /* runtime tx_buff_idx starts at thread_id * SQ_DEPTH (traditionalfair share) or just start_idx */
    app_ctx->dt_ctx.tx_buff_idx = (data_from_host->thread_id * (1 << LOG_SQ_DEPTH)) % app_ctx->shared_sq_data_count;

    for (uint32_t i = start_idx; i < end_idx; i++) {
        uint8_t *tx_buff = (uint8_t *)dpa_tx_buff_base + (i << LOG_WQD_CHUNK_BSIZE);
        
        struct eth_hdr *eth = (struct eth_hdr *)tx_buff;
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(tx_buff + sizeof(struct eth_hdr));
        struct udp_hdr *udp = (struct udp_hdr *)(tx_buff + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr));
        uint8_t *payload = (uint8_t *)(tx_buff + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr));
        
        /* Destination MAC (Broadcast) */
        memset(eth->dst_mac, 0xFF, 6);
        /* Source MAC */
        memcpy(eth->src_mac, app_ctx->device_mac, 6);
        /* Ethertype (IP) */
        eth->eth_type = 0x0008; // 0x0800 swapped for LE? No, let's assume BE as typical network. 
        
        /* IP Header (minimal) */
        ip->ver_ihl = 0x45;
        ip->tos = 0x00;
        
        uint16_t ip_len = sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + app_ctx->payload_size;
        ip->total_len = (ip_len >> 8) | (ip_len << 8); // bswap

        ip->id = (i >> 8) | (i << 8); // Unique ID per slot
        ip->frag_off = 0;
        ip->ttl = 64;
        ip->next_proto_id = 17; // UDP
        ip->hdr_checksum = 0;
        ip->src_addr = 0x01010101; // 1.1.1.1
        ip->dst_addr = data_from_host->dst_ip;
        
        /* UDP Header */
        /* UDP Header */
        udp->src_port = (data_from_host->udp_sport >> 8) | (data_from_host->udp_sport << 8);
        udp->dst_port = (data_from_host->udp_dport >> 8) | (data_from_host->udp_dport << 8);
        uint16_t udp_len = sizeof(struct udp_hdr) + app_ctx->payload_size;
        udp->dgram_len = (udp_len >> 8) | (udp_len << 8);
        udp->dgram_cksum = 0;

        /* Payload - Unique per slot */
        memset(payload, 0xAA + (uint8_t)(i & 0xFF), app_ctx->payload_size);
    }
    
    if (app_ctx->sq_on_host) {
        __dpa_thread_window_writeback();
    }
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	__dpa_thread_fence(__DPA_MMIO, __DPA_W, __DPA_W);
}

/* process packet - read it, swap MAC addresses, modify it, create a send WQE and send it back. */
/* process packet - read it, swap MAC addresses, modify it, create a send WQE and send it back. */
static void process_packet(struct thread_context *app_ctx, uint32_t ce)
{
	/* TX packet handling variables */
	union flexio_dev_sqe_seg *swqe;

	/* Fixed Size of the data for now: 64 bytes */
	uint32_t data_sz = sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + app_ctx->payload_size;
    app_ctx->bytes_count += data_sz;

	/* Take first segment for SQ WQE (3 segments will be used) */
	swqe = get_next_sqe(&app_ctx->sq_ctx, SQ_IDX_MASK);

	/* Fill out 1-st segment (Control) */
	flexio_dev_swqe_seg_ctrl_set(swqe, app_ctx->sq_ctx.sq_pi, app_ctx->sq_ctx.sq_number,
				     ce, FLEXIO_CTRL_SEG_SEND_EN);

	/* Fill out 2-nd segment (Ethernet) */
	swqe = get_next_sqe(&app_ctx->sq_ctx, SQ_IDX_MASK);
	flexio_dev_swqe_seg_eth_set(swqe, 1 << 14, 0, 0, NULL); // highest bit is L4 checksum offload, second highest bit is L3 checksum offload

	/* Fill out 3-rd segment (Data) */
	swqe = get_next_sqe(&app_ctx->sq_ctx, SQ_IDX_MASK);
    /* Get the next buffer in round-robin fashion from the shared pool */
    char *tx_data = (char *)(app_ctx->dt_ctx.sq_tx_buff) + (app_ctx->dt_ctx.tx_buff_idx << LOG_WQD_CHUNK_BSIZE);
    app_ctx->dt_ctx.tx_buff_idx++;
    if (app_ctx->dt_ctx.tx_buff_idx >= app_ctx->shared_sq_data_count) {
        app_ctx->dt_ctx.tx_buff_idx = 0;
    }
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, app_ctx->lkey, (uint64_t)tx_data);
    if (app_ctx->sq_on_host) {
        __dpa_thread_window_writeback();
    }

	/* Send WQE is 4 WQEBBs need to skip the 4-th segment */
	swqe = get_next_sqe(&app_ctx->sq_ctx, SQ_IDX_MASK);

	/* Ring DB */
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_qp_sq_ring_db(++app_ctx->sq_ctx.sq_pi, app_ctx->sq_ctx.sq_number);
    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
}

/* Initialization RPC: Sets up context and sends first batch */
/* Initialization RPC Phase 1: Context Init */
flexio_dev_rpc_handler_t flexio_pp_init_context;
__dpa_rpc__ uint64_t flexio_pp_init_context(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data *data_from_host = (void *)thread_arg;
	struct thread_context *app_ctx;

	if (data_from_host->thread_id >= MAX_THREADS) return 0;
	app_ctx = &__thread_contexts[data_from_host->thread_id];

	app_ctx_init_context(data_from_host, app_ctx);
	return 0;
}

/* Initialization RPC Phase 2: Buffer Init */
flexio_dev_rpc_handler_t flexio_pp_init_buffers;
__dpa_rpc__ uint64_t flexio_pp_init_buffers(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data *data_from_host = (void *)thread_arg;
	struct thread_context *app_ctx = &__thread_contexts[data_from_host->thread_id];

	app_ctx_init_buffers(data_from_host, app_ctx);
	return 0;
}

/* Initialization RPC Phase 3: Start App (sending) */
flexio_dev_rpc_handler_t flexio_pp_start_app;
__dpa_rpc__ uint64_t flexio_pp_start_app(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data *data_from_host = (void *)thread_arg;
	struct thread_context *app_ctx = &__thread_contexts[data_from_host->thread_id];

	data_from_host->not_first_run = 1;

    /* Fill SQ with first batch to kickstart completions */
    for (int i = 0; i < SQ_BATCH_SIZE; i++) {
        uint32_t ce = (i == SQ_BATCH_SIZE - 1) ? MLX5_CTRL_SEG_CE_CQE_ALWAYS : MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR;
        app_ctx->packets_count++;
        process_packet(app_ctx, ce);
    }
    
    /* Arm SQ CQ */
    flexio_dev_cq_arm(app_ctx->sq_cq_ctx.cq_idx, app_ctx->sq_cq_ctx.cq_number);
    
    return app_ctx->packets_count;
}

/* completions-triggered event handler */
flexio_dev_event_handler_t flexio_pp_dev;
__dpa_global__ void flexio_pp_dev(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data *data_from_host = (void *)thread_arg;
	struct thread_context *app_ctx = &__thread_contexts[data_from_host->thread_id];
    uint32_t sq_depth = 1 << LOG_SQ_DEPTH;

	/* Drain CQEs and replenish SQ in batches. */
	for (int iter = 0; iter < 1024; iter++) {
		int progress = 0;

		/* 1. Poll SQ completions to reclaim credits. */
		while (flexio_dev_cqe_get_owner(app_ctx->sq_cq_ctx.cqe) != app_ctx->sq_cq_ctx.cq_hw_owner_bit) {
			app_ctx->sq_wqe_counter = (uint32_t)flexio_dev_cqe_get_wqe_counter(app_ctx->sq_cq_ctx.cqe);
			com_step_cq(&app_ctx->sq_cq_ctx);
			progress = 1;
		}

		/* 2. Replenish SQ while space is available. */
		while ((uint16_t)(app_ctx->sq_ctx.sq_pi - app_ctx->sq_wqe_counter) < sq_depth - SQ_BATCH_SIZE) {
			for (int i = 0; i < SQ_BATCH_SIZE; i++) {
				uint32_t ce = (i == SQ_BATCH_SIZE - 1) ? MLX5_CTRL_SEG_CE_CQE_ALWAYS : MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR;
				process_packet(app_ctx, ce);
			}
			app_ctx->packets_count += SQ_BATCH_SIZE;
		}

		/* Exit if no new completions were found (drained). */
		if (!progress) break;
	}

	/* 3. Re-arm and reschedule */
	flexio_dev_cq_arm(app_ctx->sq_cq_ctx.cq_idx, app_ctx->sq_cq_ctx.cq_number);
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
