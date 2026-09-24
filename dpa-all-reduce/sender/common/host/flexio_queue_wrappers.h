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

#ifndef __COM_FLEXIO_QUEUE_WRAPPERS_H__
#define __COM_FLEXIO_QUEUE_WRAPPERS_H__

/* Flex IO SDK host side API header. */
#include <libflexio/flexio.h>
#include <libflexio/flexio.h>
#include <memory>
#include <variant>

/* Forward declarations */
class FlexioCQ;

/* Transfer structures re-definition to avoid dependency on app specific headers if possible, 
 * or we should include the common header. For now, let's use the layout compatible with 
 * what was in the host file, but ideally these should be in a shared header.
 * The user mentions "The structs are defined by a common header which both sides may use."
 * We'll use the types from that header if we can include it, but it might be app specific.
 * For this common library, we will manage the DPA addresses and let the app copy them 
 * to its own transfer structs.
 */

class FlexioMKey {
public:
    FlexioMKey(struct flexio_process *process, struct ibv_pd *pd, flexio_uintptr_t daddr, size_t len);
    ~FlexioMKey();

    /* Delete copy */
    FlexioMKey(const FlexioMKey&) = delete;
    FlexioMKey& operator=(const FlexioMKey&) = delete;

    struct flexio_mkey *get_mkey() const { return mkey; }
    uint32_t get_id() const { return mkey ? flexio_mkey_get_id(mkey) : 0; }

private:
    struct flexio_mkey *mkey = nullptr;
};

class FlexioMemory {
public:
    virtual ~FlexioMemory() = default;
    virtual flexio_uintptr_t get_daddr() const = 0;
    virtual uint32_t get_mkey_id() const = 0;
    virtual bool isValid() const = 0;
};

class DPAMemory : public FlexioMemory {
public:
    DPAMemory() = default;
    DPAMemory(struct flexio_process *process, size_t size, struct ibv_pd *pd = nullptr);
    ~DPAMemory();

    DPAMemory(DPAMemory&& other) noexcept;
    DPAMemory& operator=(DPAMemory&& other) noexcept;
    DPAMemory(const DPAMemory&) = delete;
    DPAMemory& operator=(const DPAMemory&) = delete;

    flexio_uintptr_t get_daddr() const override { return daddr; }
    uint32_t get_mkey_id() const override { return mkey ? flexio_mkey_get_id(mkey) : 0; }
    bool isValid() const override { return daddr != 0; }

private:
    struct flexio_process *process = nullptr;
    flexio_uintptr_t daddr = 0;
    struct flexio_mkey *mkey = nullptr;
};

class HostMemory : public FlexioMemory {
public:
    HostMemory() = default;
    HostMemory(struct ibv_pd *pd, size_t size, size_t alignment);
    ~HostMemory();

    HostMemory(HostMemory&& other) noexcept;
    HostMemory& operator=(HostMemory&& other) noexcept;
    HostMemory(const HostMemory&) = delete;
    HostMemory& operator=(const HostMemory&) = delete;
    
    flexio_uintptr_t get_daddr() const override { return daddr; }
    uint32_t get_mkey_id() const override { return mr ? mr->lkey : 0; }
    bool isValid() const override { return host_ptr != nullptr; }

private:
    flexio_uintptr_t daddr = 0;
    void* host_ptr = nullptr;
    struct ibv_mr* mr = nullptr;
};

class FlexioCQ {
public:
    FlexioCQ(struct flexio_process *process, struct flexio_uar *uar, 
             struct flexio_event_handler *event_handler, int log_depth, int element_type);
    ~FlexioCQ();

    /* Delete copy */
    FlexioCQ(const FlexioCQ&) = delete;
    FlexioCQ& operator=(const FlexioCQ&) = delete;

    struct flexio_cq *get_cq() const { return cq; }
    flexio_uintptr_t get_dbr_daddr() const { return dbr_daddr; }
    flexio_uintptr_t get_ring_daddr() const { return ring_daddr; }
    uint32_t get_cq_num() const;

private:
    struct flexio_process *process = nullptr;
    struct flexio_cq *cq = nullptr;
    
    /* DPA memory addresses */
    flexio_uintptr_t dbr_daddr = 0;
    flexio_uintptr_t ring_daddr = 0;
};

class FlexioSQ {
public:
    /* Creates SQ and its CQ. host_wq_data specifies if data buffer is on host */
    FlexioSQ(struct flexio_process *process, struct flexio_uar *uar, int log_depth, struct ibv_pd *pd, int log_data_bsize, struct flexio_event_handler *event_handler = nullptr, std::shared_ptr<FlexioMemory> external_mem = nullptr);
    ~FlexioSQ();

    /* Delete copy */
    FlexioSQ(const FlexioSQ&) = delete;
    FlexioSQ& operator=(const FlexioSQ&) = delete;

    struct flexio_sq *get_sq() const { return sq; }
    FlexioCQ *get_cq() const { return b_cq.get(); }
    
    flexio_uintptr_t get_wqd_daddr() const { return wqd_daddr; }
    flexio_uintptr_t get_wq_ring_daddr() const { return wq_ring_daddr; }
    flexio_uintptr_t get_wq_dbr_daddr() const { return wq_dbr_daddr; }
    uint32_t get_wqd_mkey_id() const;
    uint32_t get_wq_num() const;

private:

    struct flexio_process *process = nullptr;
    struct flexio_sq *sq = nullptr;
    std::unique_ptr<FlexioCQ> b_cq; /* Owned CQ */
    std::unique_ptr<FlexioMKey> wqd_mkey;

    /* Data and Ring memory */
    flexio_uintptr_t wqd_daddr = 0;
    std::shared_ptr<FlexioMemory> data_mem; 
    flexio_uintptr_t wq_ring_daddr = 0;
    flexio_uintptr_t wq_dbr_daddr = 0; 
    uint32_t external_mkey_id = 0;
};

class FlexioRQ {
public:
    /* Creates RQ and its CQ */
    FlexioRQ(struct flexio_process *process, struct flexio_uar *uar, 
             struct flexio_event_handler *event_handler, int log_depth, 
             struct ibv_pd *pd, int log_data_bsize, std::shared_ptr<FlexioMemory> external_mem = nullptr);
    ~FlexioRQ();

    /* Delete copy */
    FlexioRQ(const FlexioRQ&) = delete;
    FlexioRQ& operator=(const FlexioRQ&) = delete;

    struct flexio_rq *get_rq() const { return rq; }
    FlexioCQ *get_cq() const { return b_cq.get(); }
    
    flexio_uintptr_t get_wqd_daddr() const { return wqd_daddr; }
    flexio_uintptr_t get_wq_ring_daddr() const { return ring_mem->get_daddr(); }
    flexio_uintptr_t get_wq_dbr_daddr() const { return wq_dbr_daddr; }
    uint32_t get_wqd_mkey_id() const;
    uint32_t get_wq_num() const;

private:

    struct flexio_process *process = nullptr;
    struct flexio_rq *rq = nullptr;
    std::unique_ptr<FlexioCQ> b_cq; /* Owned CQ */
    std::unique_ptr<FlexioMKey> wqd_mkey;

    /* DPA memory addresses */
    flexio_uintptr_t wqd_daddr = 0;
    std::shared_ptr<FlexioMemory> data_mem; /* Data memory */
    std::unique_ptr<FlexioMemory> ring_mem; /* Ring memory */
    flexio_uintptr_t wq_dbr_daddr = 0; /* Doorbell memory */
};

class CommandQueue {
public:
    CommandQueue(struct flexio_process *proc, int num_threads, int batch_size);
    ~CommandQueue();

    /* Delete copy */
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    void add_task(flexio_func_t *func, flexio_uintptr_t ddata);
    void run();
    bool is_empty();
    bool wait_until_empty(int timeout_ms);

private:
    struct flexio_cmdq *cmd_q = nullptr;
    struct flexio_process *process = nullptr;
};

#endif
