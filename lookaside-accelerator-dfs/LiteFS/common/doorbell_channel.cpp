#include "doorbell_channel.h"
#include "doca_log.h"
#include <stdexcept>

DOCA_LOG_REGISTER(LITEFS_DOORBELL_CHANNEL);

void DoorbellRingerChannel::send_doorbell(uint64_t seq, uint64_t start, uint64_t length, bool last_segment) {
    size_t idx = 0; DoorbellMsg* slot = nullptr;
    if (!try_alloc_send(idx, slot)) {
        throw std::runtime_error("DoorbellRingerChannel: send ring exhausted");
    }
    slot->magic = LITEFS_DOORBELL_MAGIC;
    slot->seq = seq;
    slot->start = start;
    slot->length = length;
    slot->end_segment = last_segment ? 1 : 0;
    post_send(idx, sizeof(DoorbellMsg));
    DOCA_LOG_DBG("DoorbellRingerChannel: SEND doorbell: seq=%lu start=%lu len=%lu end=%u", seq, start, length, (unsigned)slot->end_segment);
}

bool DoorbellRingerChannel::poll_ack(AckMsg& out) {
    bool got = poll_one(out);
    if (got) {
        DOCA_LOG_DBG("DoorbellRingerChannel: RECV ack: seq=%lu len=%lu", out.seq, out.length);
    }
    return got;
}

bool DoorbellListenerChannel::poll(DoorbellMsg& out) {
    bool got = poll_one(out);
    if (got) {
        DOCA_LOG_DBG("DoorbellListenerChannel: RECV doorbell: seq=%lu start=%lu len=%lu end=%u", out.seq, out.start, out.length, (unsigned)out.end_segment);
    }
    return got;
}

void DoorbellListenerChannel::ack(uint64_t seq, uint64_t length) {
    size_t idx = 0; AckMsg* slot = nullptr;
    if (!try_alloc_send(idx, slot)) {
        throw std::runtime_error("DoorbellListenerChannel: ack send ring exhausted");
    }
    slot->magic = LITEFS_ACK_MAGIC;
    slot->seq = seq;
    slot->length = length;
    post_send(idx, sizeof(AckMsg));
    last_seq_.store(seq, std::memory_order_release);
    DOCA_LOG_DBG("DoorbellListenerChannel: SEND ack: seq=%lu len=%lu", seq, length);
}
