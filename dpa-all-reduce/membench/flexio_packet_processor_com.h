/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __FLEXIO_PACKET_PROCESSOR_COM_H__
#define __FLEXIO_PACKET_PROCESSOR_COM_H__

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Helper macro for device pointer definition */
#if defined(__x86_64__) || defined(__aarch64__)
#define DevP(type) flexio_uintptr_t
#elif defined(__riscv)
#define DevP(type) type *
#else
#define DevP(type)
#endif

/* Memory Types */
#define HOST_MEM 0
#define PRIVATE_MEM 1

/* Operation Types */
#define OP_READ 0
#define OP_WRITE 1

#define EXPAND_SIZE 1

/* Argument structure for memory benchmark */
struct MemoryArg {
	int stream_id;
	uint32_t window_id;
	uint32_t mkey;
	int mem_type; // 1 = HOST, 2 = PRIVATE
    int op; // 0 = WRITE, 1 = READ
	size_t size; // Buffer size in bytes
	int stride;
	size_t loop;
	uint64_t haddr; // Host address (for data)
	uint64_t ddata; // Device address (for data)
	uint64_t completion_flag; // Device address (Host Mapped)
	uint64_t progress_daddr; // Device address (Host Mapped) for progress
	uint64_t stop_daddr; // Device address (Host Mapped) for stop signal
} __attribute__((aligned(8)));

#define MAX_THREADS 256

#if defined(__cplusplus)
}
#endif

#endif /* __FLEXIO_PACKET_PROCESSOR_COM_H__ */
