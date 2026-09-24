#ifndef LITEFS_DOORBELL_H
#define LITEFS_DOORBELL_H

#include <cstdint>

#define LITEFS_DOORBELL_MAGIC 0x44424C4Cu /* 'DBLL' */
#define LITEFS_ACK_MAGIC      0x41434B21u /* 'ACK!' */

struct DoorbellMsg {
    uint32_t magic;    // LITEFS_DOORBELL_MAGIC
    uint32_t reserved; // alignment
    uint64_t seq;      // monotonically increasing
    uint64_t start;    // start offset in Log Area (bytes)
    uint64_t length;   // total bytes in this doorbelled segment
    uint8_t  end_segment;  // indicate if this is the last segment of the doorbell unit
    uint8_t  pad[31];  // pad to 64B
};

struct AckMsg {
    uint32_t magic;    // LITEFS_ACK_MAGIC
    uint32_t reserved; // alignment
    uint64_t seq;      // acked sequence
    uint64_t length;   // total bytes in this ack unit
    uint8_t  pad[32];  // pad to 64B
};

#endif // LITEFS_DOORBELL_H


