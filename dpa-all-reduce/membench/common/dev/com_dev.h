/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Header file with utilities for DPA.
 */

#ifndef __COM_DEV_H__
#define __COM_DEV_H__

/* Flex IO SDK device side version API header. */
#include <libflexio-dev/flexio_dev_ver.h>

/* Set current version of FLEXIO_DEV_VER_USED. */
#define FLEXIO_DEV_VER_USED FLEXIO_DEV_VER(25, 7, 0)

/* Flex IO SDK device side API header. */
#include <libflexio-dev/flexio_dev.h>

#ifndef NULL
#define NULL (void *)0
#endif

/* Convert logarithm to value */
#define L2V(l) (1UL << (l))
/* Convert logarithm to mask */
#define L2M(l) (L2V(l) - 1)

#define device_assert(x) if (!(x)) { flexio_dev_print("Assertion failed: " #x); }

/* Helper function to get Host buffer address on DPA */
flexio_uintptr_t get_host_buffer(uint32_t window_id, uint32_t mkey, void *haddr);

#endif
