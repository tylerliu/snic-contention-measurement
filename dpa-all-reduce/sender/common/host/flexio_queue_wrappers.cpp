/*
 * Copyright (c) 2025 Tyler Liu. All rights reserved.
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <netinet/in.h>
#include <infiniband/mlx5dv.h>
#include <stdexcept>
#include <chrono>
#include <thread>

/* Set current version of FLEXIO_VER_USED for standalone compilation. */
#ifndef FLEXIO_VER_USED
#define FLEXIO_VER_USED FLEXIO_VER(25, 7, 0)
#endif
#include "flexio_queue_wrappers.h"

/* Helper Constants */
#define CQE_BSIZE 64
/* LOG_Q_QENTRY_BSIZE for RQ and SQ is usually 6 (64 bytes) or 11 (2048 bytes) depending on usage */
/* In the original app, LOG_Q_DATA_ENTRY_BSIZE was 11 and LOG_SQ_WQE_BSIZE/LOG_RQ_WQE_BSIZE was 6 */

#define LOG_WQ_WQE_BSIZE 6
#define WQ_WQE_BSIZE (1UL << LOG_WQ_WQE_BSIZE)

/* Private Helper Functions */

static int cq_mem_alloc(struct flexio_process *process, int log_depth, flexio_uintptr_t *dbr_daddr, flexio_uintptr_t *ring_daddr)
{
	struct mlx5_cqe64 *cq_ring_src;
	struct mlx5_cqe64 *cqe;
	__be32 dbr[2] = { 0, 0 };
	uint32_t i;
	size_t depth = 1UL << log_depth;
	size_t cq_bsize = depth * CQE_BSIZE;

	if (flexio_copy_from_host(process, dbr, sizeof(dbr), dbr_daddr)) {
		printf("Failed to allocate CQ DBR memory on DPA heap.\n");
		return -1;
	}

	cq_ring_src = (struct mlx5_cqe64 *) calloc(depth, CQE_BSIZE);
	if (!cq_ring_src) {
		printf("Failed to allocate memory for cq_ring_src.\n");
		return -1;
	}

	for (i = 0, cqe = cq_ring_src; i < depth; i++)
		mlx5dv_set_cqe_owner(cqe++, 1);

	if (flexio_copy_from_host(process, cq_ring_src, cq_bsize, ring_daddr)) {
		printf("Failed to allocate CQ ring memory on DPA heap.\n");
		free(cq_ring_src);
		return -1;
	}

	free(cq_ring_src);
	return 0;
}

/* DPAMemory Implementation */

DPAMemory::DPAMemory(struct flexio_process *process, size_t size, struct ibv_pd *pd) : process(process) {
    if (flexio_buf_dev_alloc(process, size, &daddr)) {
        throw std::runtime_error("Failed to allocate DPA memory");
    }
    if (pd) {
        struct flexio_mkey_attr mkey_attr = {0};
        mkey_attr.pd = pd;
        mkey_attr.daddr = daddr;
        mkey_attr.len = size;
        mkey_attr.access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        if (flexio_device_mkey_create(process, &mkey_attr, &mkey)) {
             flexio_buf_dev_free(process, daddr);
             daddr = 0;
             throw std::runtime_error("Failed to create MKey for DPA memory");
        }
    }
}

DPAMemory::~DPAMemory() {
    if (mkey) flexio_device_mkey_destroy(mkey);
    if (daddr && process) {
        flexio_buf_dev_free(process, daddr);
    }
}

DPAMemory::DPAMemory(DPAMemory&& other) noexcept 
    : process(other.process), daddr(other.daddr), mkey(other.mkey) {
    other.daddr = 0;
    other.process = nullptr;
    other.mkey = nullptr;
}

DPAMemory& DPAMemory::operator=(DPAMemory&& other) noexcept {
    if (this != &other) {
        if (mkey) flexio_device_mkey_destroy(mkey);
        if (daddr && process) flexio_buf_dev_free(process, daddr);
        process = other.process;
        daddr = other.daddr;
        mkey = other.mkey;
        other.daddr = 0;
        other.process = nullptr;
        other.mkey = nullptr;
    }
    return *this;
}

/* HostMemory Implementation */

HostMemory::HostMemory(struct ibv_pd *pd, size_t size, size_t alignment) {
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

HostMemory::~HostMemory() {
    if (mr) ibv_dereg_mr(mr);
    if (host_ptr) free(host_ptr);
}

HostMemory::HostMemory(HostMemory&& other) noexcept 
    : daddr(other.daddr), host_ptr(other.host_ptr), mr(other.mr) {
    other.daddr = 0;
    other.host_ptr = nullptr;
    other.mr = nullptr;
}

HostMemory& HostMemory::operator=(HostMemory&& other) noexcept {
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

/* FlexioCQ Implementation */

FlexioCQ::FlexioCQ(struct flexio_process *process, 
                   struct flexio_uar *uar, 
                   struct flexio_event_handler *event_handler,
                   int log_depth,
                   int element_type) : process(process)
{
    if (cq_mem_alloc(process, log_depth, &dbr_daddr, &ring_daddr)) {
        throw std::runtime_error("Failed to allocate CQ memory");
    }

    struct flexio_cq_attr cq_attr = {0};
    cq_attr.log_cq_depth = log_depth;
    cq_attr.element_type = element_type;
    cq_attr.uar_id = flexio_uar_get_id(uar);
    cq_attr.cq_dbr_daddr = dbr_daddr;
    cq_attr.cq_ring_qmem.daddr = ring_daddr;

    if (element_type == FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD && event_handler) {
        cq_attr.thread = flexio_event_handler_get_thread(event_handler);
    }

    if (flexio_cq_create(process, nullptr, &cq_attr, &cq)) {
        throw std::runtime_error("Failed to create Flex IO CQ");
    }
}

FlexioCQ::~FlexioCQ()
{
    if (cq) flexio_cq_destroy(cq);
    if (dbr_daddr) flexio_buf_dev_free(process, dbr_daddr);
    if (ring_daddr) flexio_buf_dev_free(process, ring_daddr);
}


uint32_t FlexioCQ::get_cq_num() const {
    return flexio_cq_get_cq_num(cq);
}

/* FlexioMKey Implementation */

FlexioMKey::FlexioMKey(struct flexio_process *process, 
                       struct ibv_pd *pd, 
                       flexio_uintptr_t daddr, 
                       size_t len)
{
    struct flexio_mkey_attr mkey_attr = {0};

    mkey_attr.pd = pd;
    mkey_attr.daddr = daddr;
    mkey_attr.len = len;
    mkey_attr.access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    
    if (flexio_device_mkey_create(process, &mkey_attr, &mkey)) {
        throw std::runtime_error("Failed to create Flex IO MKey");
    }
}

FlexioMKey::~FlexioMKey()
{
    if (mkey) flexio_device_mkey_destroy(mkey);
}

/* FlexioSQ Implementation */


FlexioSQ::FlexioSQ(struct flexio_process *process,
                   struct flexio_uar *uar,
                   int log_depth,
                   struct ibv_pd *pd,
                   int log_data_bsize,
                   struct flexio_event_handler *event_handler,
                   std::shared_ptr<FlexioMemory> external_mem) : process(process)
{
    size_t depth = 1UL << log_depth;

    /* Create CQ for SQ associated (DPA Thread if event_handler is provided) */
    int element_type = event_handler ? FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD : FLEXIO_CQ_ELEMENT_TYPE_NON_DPA_CQ;
    b_cq = std::make_unique<FlexioCQ>(process, uar, event_handler, log_depth, element_type);

    /* Set or Alloc SQ memory for Data */
    if (external_mem) {
        data_mem = external_mem;
        wqd_daddr = data_mem->get_daddr();
    } else {
        size_t data_size = depth * (1UL << log_data_bsize);
        data_mem = std::make_shared<DPAMemory>(process, data_size, pd);
        wqd_daddr = data_mem->get_daddr();
    }

    if (flexio_buf_dev_alloc(process, depth * WQ_WQE_BSIZE, &wq_ring_daddr)) {
        throw std::runtime_error("Failed to allocate SQ ring memory");
    }

    struct flexio_wq_attr sq_attr = {0};
    sq_attr.log_wq_depth = log_depth;
    sq_attr.uar_id = flexio_uar_get_id(uar);
    sq_attr.wq_ring_qmem.daddr = wq_ring_daddr;
    sq_attr.pd = pd;

    if (flexio_sq_create(process, nullptr, b_cq->get_cq_num(), &sq_attr, &sq)) {
        throw std::runtime_error("Failed to create Flex IO SQ");
    }
}

FlexioSQ::~FlexioSQ()
{
    if (sq) flexio_sq_destroy(sq);
    if (wq_ring_daddr) flexio_buf_dev_free(process, wq_ring_daddr);
}

uint32_t FlexioSQ::get_wqd_mkey_id() const {
    if (data_mem) return data_mem->get_mkey_id();
    return 0;
}

uint32_t FlexioSQ::get_wq_num() const {
    return flexio_sq_get_wq_num(sq);
}


/* FlexioRQ Implementation */

static int init_dpa_rq_ring(struct flexio_process *process, flexio_uintptr_t wqd_daddr, uint32_t mkey_id, flexio_uintptr_t ring_daddr, int log_depth, int log_data_bsize)
{
    size_t depth = 1UL << log_depth;
    size_t ring_bsize = depth * WQ_WQE_BSIZE;
    size_t data_entry_bsize = 1UL << log_data_bsize;
	struct mlx5_wqe_data_seg *rx_wqes;
	struct mlx5_wqe_data_seg *dseg;
	int retval = 0;
	uint32_t i;
    flexio_uintptr_t curr_daddr = wqd_daddr;

	rx_wqes = (struct mlx5_wqe_data_seg *) calloc(1, ring_bsize);
	if (!rx_wqes) return -1;

	for (i = 0, dseg = rx_wqes; i < depth; i++, dseg++) {
		mlx5dv_set_data_seg(dseg, data_entry_bsize, mkey_id, curr_daddr);
		curr_daddr += data_entry_bsize;
	}

	if (flexio_host2dev_memcpy(process, rx_wqes, ring_bsize, ring_daddr)) {
		retval = -1;
	}

	free(rx_wqes);
	return retval;
}

FlexioRQ::FlexioRQ(struct flexio_process *process,
                   struct flexio_uar *uar,
                   struct flexio_event_handler *event_handler,
                   int log_depth,
                   struct ibv_pd *pd,
                   int log_data_bsize,
                   std::shared_ptr<FlexioMemory> external_mem) : process(process)
{
    size_t depth = 1UL << log_depth;

    /* Create CQ for RQ (DPA Thread) */
    b_cq = std::make_unique<FlexioCQ>(process, uar, event_handler, log_depth, FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD);

    /* Set or Alloc RQ memory for Data */
    if (external_mem) {
        data_mem = external_mem;
        wqd_daddr = data_mem->get_daddr();
    } else {
        size_t data_size = depth * (1UL << log_data_bsize);
        data_mem = std::make_shared<DPAMemory>(process, data_size, pd);
        wqd_daddr = data_mem->get_daddr();
    }

    /* Alloc RQ ring */
    ring_mem = std::make_unique<DPAMemory>(process, depth * WQ_WQE_BSIZE);
    if (!ring_mem->isValid()) throw std::runtime_error("Invalid DPA Memory for ring");
    
    /* Alloc RQ DBR */
    __be32 dbr[2] = { 0, 0 };
    dbr[0] = htobe32(depth & 0xffff);
    if (flexio_copy_from_host(process, dbr, sizeof(dbr), &wq_dbr_daddr)) {
         throw std::runtime_error("Failed to allocate RQ DBR memory"); 
    }

    /* Init Ring */
    if (init_dpa_rq_ring(process, wqd_daddr, get_wqd_mkey_id(), get_wq_ring_daddr(), log_depth, log_data_bsize)) {
        throw std::runtime_error("Failed to init RQ Ring");
    }

    struct flexio_wq_attr rq_attr = {0};
    rq_attr.log_wq_depth = log_depth;
    rq_attr.pd = pd;
    rq_attr.wq_dbr_qmem.memtype = FLEXIO_MEMTYPE_DPA;
    rq_attr.wq_dbr_qmem.daddr = wq_dbr_daddr;
    rq_attr.wq_ring_qmem.daddr = get_wq_ring_daddr();

    if (flexio_rq_create(process, nullptr, b_cq->get_cq_num(), &rq_attr, &rq)) {
        throw std::runtime_error("Failed to create Flex IO RQ");
    }
}

FlexioRQ::~FlexioRQ()
{
    if (rq) flexio_rq_destroy(rq);
    if (wq_dbr_daddr) flexio_buf_dev_free(process, wq_dbr_daddr);
}

uint32_t FlexioRQ::get_wqd_mkey_id() const {
    if (data_mem) return data_mem->get_mkey_id();
    return 0;
}

uint32_t FlexioRQ::get_wq_num() const {
    return flexio_rq_get_wq_num(rq);
}
/* CommandQueue Implementation */

CommandQueue::CommandQueue(struct flexio_process *proc, int num_threads, int batch_size) : process(proc) {
    flexio_cmdq_attr cmdq_attr = {0};
    cmdq_attr.workers = num_threads;
    cmdq_attr.batch_size = batch_size;

    if (flexio_cmdq_create(process, &cmdq_attr, &cmd_q) != FLEXIO_STATUS_SUCCESS) {
        throw std::runtime_error("Failed to create command queue");
    }
}

CommandQueue::~CommandQueue() {
    if (cmd_q) flexio_cmdq_destroy(cmd_q);
}

void CommandQueue::add_task(flexio_func_t *func, flexio_uintptr_t ddata) {
    if (flexio_cmdq_task_add(cmd_q, func, ddata) != FLEXIO_STATUS_SUCCESS) {
         throw std::runtime_error("Failed to add task to command queue");
    }
}

void CommandQueue::run() {
    if (flexio_cmdq_state_running(cmd_q) != FLEXIO_STATUS_SUCCESS) {
         throw std::runtime_error("Failed to run command queue");
    }
}

bool CommandQueue::is_empty() {
    return flexio_cmdq_is_empty(cmd_q);
}

bool CommandQueue::wait_until_empty(int timeout_ms) {
    auto start = std::chrono::high_resolution_clock::now();
    while (!is_empty()) {
        auto current = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(current - start).count() > timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
