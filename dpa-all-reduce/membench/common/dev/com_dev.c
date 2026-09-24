/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Source file with utilities for DPA.
 */

#include "com_dev.h"
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <stddef.h>
#include <dpaintrin.h>

flexio_uintptr_t get_host_buffer(uint32_t window_id, uint32_t mkey, void *haddr) {
	flexio_uintptr_t host_buffer = 0;
	flexio_dev_status_t ret;

	ret = flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, (uint16_t)window_id, mkey);
    if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
        flexio_dev_print("Window config failed: %d\n", ret);
        return 0;
    }

	ret = flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0, (uint64_t)haddr, &host_buffer);
    if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
        flexio_dev_print("Window acquire failed: %d, addr: 0x%lx\n", ret, (uint64_t)haddr);
        return 0;
    }
	return host_buffer;
}
