#ifndef LITEFS_DOORBELL_CHANNEL_H
#define LITEFS_DOORBELL_CHANNEL_H

#include <cstdint>
#include <infiniband/verbs.h>
#include <string>
#include <stdexcept>
#include <unistd.h>
#include <memory>
#include <doca_log.h>
#include <atomic>

#include "verbs_helper.h"
#include "doorbell.h"
#include "ring_buffer.h"

// Generic actor that manages a SEND ring and a RECV ring backed by single MRs
template<typename SendMsg, typename RecvMsg, uint32_t RecvMagic>
class DoorbellActor {
public:
    explicit DoorbellActor(VerbsManager& verbs, size_t send_depth, size_t recv_depth)
     : verbs_(verbs)
     , send_free_ring_(send_depth + 1)
     , send_capacity_(send_depth)
     , recv_capacity_(recv_depth) {
        assert((send_capacity_ & (send_capacity_ + 1)) == 0 && "send_capacity_ must be a power of two - 1");
    }

    void setup_after_handshake() {
        if (send_capacity_ > 0) {
            send_ring_.reset(new SendMsg[send_capacity_]());
            send_ring_mr_ = verbs_.RegisterMemory(send_ring_.get(), send_capacity_ * sizeof(SendMsg));
            // Initialize SPSC ring for send free indices
            for (size_t i = 0; i < send_capacity_; i++) {
                assert(send_free_ring_.try_push(i) && "Failed to push send free index");
            }
        }
        if (recv_capacity_ > 0) {
            recv_ring_.reset(new RecvMsg[recv_capacity_]());
            recv_ring_mr_ = verbs_.RegisterMemory(recv_ring_.get(), recv_capacity_ * sizeof(RecvMsg));
            for (size_t i = 0; i < recv_capacity_; i++) {
                verbs_.PostRecv(recv_ring_mr_, &recv_ring_[i], sizeof(RecvMsg));
            }
        }
    }

    bool try_alloc_send(size_t& idx, SendMsg*& slot) {
        if (!send_free_ring_.try_pop(idx)) return false;
        slot = &send_ring_[idx];
        return true;
    }

    void post_send(size_t idx, size_t len) {
        SendMsg* slot = &send_ring_[idx];
        verbs_.PostSend(send_ring_mr_, slot, len, IBV_SEND_INLINE | IBV_SEND_IP_CSUM);
    }

    // Poll CQ; reclaim SENDs; deliver one RECV into 'out' when available
    bool poll_one(RecvMsg& out) {
        struct ibv_wc wc = {};
        while (verbs_.PollCompletion(&wc) > 0) {
            if (wc.status != IBV_WC_SUCCESS) { 
                throw std::runtime_error("DoorbellActor: CQE error status=" + std::to_string(wc.status)
                                        + " opcode=" + std::to_string(wc.opcode)
                                        + " wr_id=" + std::to_string(wc.wr_id)
                                        + " vendor_err=" + std::to_string(wc.vendor_err)
                                        + " byte_len=" + std::to_string(wc.byte_len));
            }
            if (wc.opcode == IBV_WC_SEND && send_capacity_ > 0) {
                SendMsg* addr = reinterpret_cast<SendMsg*>((uintptr_t)wc.wr_id);
                size_t idx = (size_t)(addr - send_ring_.get());
                if (idx < send_capacity_) {
                    send_free_ring_.try_push(idx);
                }
                continue;
            }
            if (wc.opcode == IBV_WC_RECV && recv_capacity_ > 0) {
                RecvMsg* addr = reinterpret_cast<RecvMsg*>((uintptr_t)wc.wr_id);
                if (wc.byte_len != sizeof(RecvMsg)) {
                    throw std::runtime_error("DoorbellActor: Unexpected RECV length=" + std::to_string(wc.byte_len)
                                            + " expected=" + std::to_string(sizeof(RecvMsg)));
                    verbs_.PostRecv(recv_ring_mr_, addr, sizeof(RecvMsg));
                    continue;
                }
                if (addr->magic != RecvMagic) {
                    throw std::runtime_error("DoorbellActor: Unexpected RECV magic=" + std::to_string(addr->magic)
                                            + " expected=" + std::to_string(RecvMagic));
                    verbs_.PostRecv(recv_ring_mr_, addr, sizeof(RecvMsg));
                    continue;
                }
                out = *addr;
                verbs_.PostRecv(recv_ring_mr_, addr, sizeof(RecvMsg));
                return true;
            }
        }
        return false;
    }

    size_t send_available() const { return send_free_ring_.approx_size(); }

protected:
    VerbsManager& verbs_;
    std::unique_ptr<SendMsg[]> send_ring_;
    std::unique_ptr<RecvMsg[]> recv_ring_;
    struct ibv_mr* send_ring_mr_ = nullptr;
    struct ibv_mr* recv_ring_mr_ = nullptr;
    SpscRing<size_t> send_free_ring_;
    size_t send_capacity_ = 0;
    size_t recv_capacity_ = 0;
};

// Ringer: send DoorbellMsg, receive AckMsg
class DoorbellRingerChannel : public DoorbellActor<DoorbellMsg, AckMsg, LITEFS_ACK_MAGIC> {
public:
    explicit DoorbellRingerChannel(VerbsManager& verbs)
        : DoorbellActor<DoorbellMsg, AckMsg, LITEFS_ACK_MAGIC>(verbs, kSendRingDepth, kAckRecvDepth) {}

    void send_doorbell(uint64_t seq, uint64_t start, uint64_t length, bool last_segment = true);

    bool poll_ack(AckMsg& out);

private:
    static constexpr size_t kAckRecvDepth = 128;
    static constexpr size_t kSendRingDepth = 63;
};

// Listener: receive DoorbellMsg, send AckMsg
class DoorbellListenerChannel : public DoorbellActor<AckMsg, DoorbellMsg, LITEFS_DOORBELL_MAGIC> {
public:
    explicit DoorbellListenerChannel(VerbsManager& verbs)
        : DoorbellActor<AckMsg, DoorbellMsg, LITEFS_DOORBELL_MAGIC>(verbs, kAckSendRingDepth, kDoorbellRecvDepth) {}

    bool poll(DoorbellMsg& out);

    void ack(uint64_t seq, uint64_t length = 0);

    uint64_t last_seq() const { return last_seq_.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> last_seq_{0};
    static constexpr size_t kDoorbellRecvDepth = 128;
    static constexpr size_t kAckSendRingDepth = 63;
};

#endif // LITEFS_DOORBELL_CHANNEL_H