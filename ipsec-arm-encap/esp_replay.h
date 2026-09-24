#ifndef ESP_REPLAY_H
#define ESP_REPLAY_H
#include <bitset>
#include <cstdint>

// Call only after authentication and plaintext structural validation.
// External synchronization is required when sharing an SA across workers.
class EspReplay {
    uint32_t highest_ = 0;
    std::bitset<256> seen_;
public:
    bool accept(uint32_t seq) {
        if (seq == 0) return false; // ESN is disabled; never accept wraparound.
        if (seq > highest_) {
            uint32_t shift = seq - highest_;
            if (shift >= seen_.size()) seen_.reset();
            else seen_ <<= shift;
            highest_ = seq;
            seen_.set(0);
            return true;
        }
        uint32_t distance = highest_ - seq;
        if (distance >= seen_.size() || seen_.test(distance)) return false;
        seen_.set(distance);
        return true;
    }
};
#endif
