#include "pause_tracker.h"

#include <doca_log.h>

DOCA_LOG_REGISTER(LITEFS_PAUSE_TRACKER);

PauseTracker::PauseTracker(const std::string& name) 
    : last_log_time_(std::chrono::high_resolution_clock::now()), thread_name_(name) {
    DOCA_LOG_INFO("[%s] PauseTracker created", thread_name_.c_str());
}

void PauseTracker::pause() {
    pause_count_.fetch_add(1, std::memory_order_relaxed);
    
    rte_pause();
    
    // Check if we should log statistics (every second)
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time_);
    
    if (elapsed.count() >= 1) {
        uint64_t current_count = pause_count_.load(std::memory_order_relaxed);
        uint64_t calls_per_second = current_count / elapsed.count();
        
        DOCA_LOG_INFO("[%s] rte_pause() calls per second: %lu", 
               thread_name_.c_str(), calls_per_second);
        
        last_log_time_ = now;
        pause_count_.store(0, std::memory_order_relaxed);
    }
}

uint64_t PauseTracker::get_total_pause_count() const {
    return pause_count_.load(std::memory_order_relaxed);
}
