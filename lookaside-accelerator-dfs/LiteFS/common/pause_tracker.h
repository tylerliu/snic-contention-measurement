#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <cstdio>
#include <rte_pause.h>

class NullPauseTracker {
public:
    virtual ~NullPauseTracker() = default;

    virtual void pause() {
        rte_pause();
    }

    virtual uint64_t get_total_pause_count() const {
        return 0;
    }
};

class PauseTracker : public NullPauseTracker {
private:
    std::atomic<uint64_t> pause_count_{0};
    std::chrono::high_resolution_clock::time_point last_log_time_;
    std::string thread_name_;

public:
    explicit PauseTracker(const std::string& name);

    void pause() override;
    uint64_t get_total_pause_count() const override;
};
