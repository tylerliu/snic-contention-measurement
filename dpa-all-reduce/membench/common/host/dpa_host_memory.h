/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES.
 * Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <libflexio/flexio.h>
#include <infiniband/verbs.h>
#include <stdexcept>
#include <memory>
#include <cstring>
#include <variant>

/* Helper class for Host Memory management with IBV registration */
class HostMemory {
public:
    flexio_uintptr_t daddr = 0;
    void* host_ptr = nullptr;
    struct ibv_mr* mr = nullptr;

    HostMemory() = default;

    HostMemory(struct ibv_pd *pd, size_t size, size_t alignment) {
        if (posix_memalign(&host_ptr, alignment, size)) {
            throw std::runtime_error("Failed to allocate host memory");
        }
        memset(host_ptr, 0, size);
        
        mr = ibv_reg_mr(pd, host_ptr, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
        if (!mr) {
            free(host_ptr);
            throw std::runtime_error("Failed to register MR");
        }
        
        daddr = (flexio_uintptr_t)host_ptr;
    }

    ~HostMemory() {
        if (mr) ibv_dereg_mr(mr);
        if (host_ptr) free(host_ptr);
    }

    HostMemory(HostMemory&& other) noexcept 
        : daddr(other.daddr), host_ptr(other.host_ptr), mr(other.mr) {
        other.daddr = 0;
        other.host_ptr = nullptr;
        other.mr = nullptr;
    }

    HostMemory& operator=(HostMemory&& other) noexcept {
        if (this != &other) {
            if (mr) ibv_dereg_mr(mr);
            if (host_ptr) free(host_ptr);
            
            daddr = other.daddr;
            host_ptr = other.host_ptr;
            mr = other.mr;
            
            other.daddr = 0;
            other.host_ptr = nullptr;
            other.mr = nullptr;
        }
        return *this;
    }
    
    // Delete copy
    HostMemory(const HostMemory&) = delete;
    HostMemory& operator=(const HostMemory&) = delete;

    bool isValid() const { return host_ptr != nullptr; }
};

/* Helper class for DPA Memory management */
class DPAMemory {
public:
    struct flexio_process *process = nullptr;
    flexio_uintptr_t daddr = 0;

    DPAMemory() {}

    DPAMemory(struct flexio_process *process, size_t size) : process(process) {
        if (flexio_buf_dev_alloc(process, size, &daddr)) {
            throw std::runtime_error("Failed to allocate DPA memory");
        }
    }

    ~DPAMemory() {
        if (daddr && process) {
            if (flexio_buf_dev_free(process, daddr) != FLEXIO_STATUS_SUCCESS) {
                printf("Error: Failed to free DPA memory 0x%lx\n", daddr);
            }
        }
    }

    DPAMemory(DPAMemory&& other) noexcept 
        : process(other.process), daddr(other.daddr) {
        other.daddr = 0;
        other.process = nullptr;
    }

    DPAMemory& operator=(DPAMemory&& other) noexcept {
        if (this != &other) {
            if (daddr && process) flexio_buf_dev_free(process, daddr);
            process = other.process;
            daddr = other.daddr;
            other.daddr = 0;
            other.process = nullptr;
        }
        return *this;
    }

    // Delete copy
    DPAMemory(const DPAMemory&) = delete;
    DPAMemory& operator=(const DPAMemory&) = delete;

    bool isValid() const { return daddr != 0; }
};

/* Helper class for FlexIO MKey management */
class FlexioMKey {
public:
    struct flexio_mkey *mkey = nullptr;

    FlexioMKey(struct flexio_process *process, struct ibv_pd *pd, flexio_uintptr_t daddr, size_t len) {
        struct flexio_mkey_attr mkey_attr = {0};
        mkey_attr.pd = pd;
        mkey_attr.daddr = daddr;
        mkey_attr.len = len;
        mkey_attr.access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE; // Relaxed permissions for benchmark
        
        if (flexio_device_mkey_create(process, &mkey_attr, &mkey)) {
            throw std::runtime_error("Failed to create Flex IO MKey");
        }
    }

    ~FlexioMKey() {
        if (mkey) flexio_device_mkey_destroy(mkey);
    }
    
    FlexioMKey(FlexioMKey&& other) noexcept : mkey(other.mkey) {
        other.mkey = nullptr;
    }
    
    FlexioMKey& operator=(FlexioMKey&& other) noexcept {
         if (this != &other) {
            if (mkey) flexio_device_mkey_destroy(mkey);
            mkey = other.mkey;
            other.mkey = nullptr;
         }
         return *this;
    }

    // Delete copy
    FlexioMKey(const FlexioMKey&) = delete;
    FlexioMKey& operator=(const FlexioMKey&) = delete;

    uint32_t get_id() const { return mkey ? flexio_mkey_get_id(mkey) : 0; }
};
