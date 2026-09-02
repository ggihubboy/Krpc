#ifndef KRPC_SHUTDOWN_STATE_H
#define KRPC_SHUTDOWN_STATE_H

#include <atomic>
#include <cstdint>

// Signal handlers only call Request(). The EventLoop owns deadline calculation
// and decides when all work drained or the grace period expired.
class ShutdownState
{
public:
    explicit ShutdownState(int grace_ms)
        : grace_ms_(grace_ms > 0 ? grace_ms : 0)
    {
    }

    void Request()
    {
        requested_.store(true, std::memory_order_release);
    }

    bool Requested() const
    {
        return requested_.load(std::memory_order_acquire);
    }

    bool ShouldStop(int pending_jobs, int64_t now_ms)
    {
        if (!Requested())
        {
            return false;
        }
        if (!deadline_initialized_)
        {
            deadline_ms_ = now_ms + grace_ms_;
            deadline_initialized_ = true;
        }
        return pending_jobs <= 0 || now_ms >= deadline_ms_;
    }

private:
    const int64_t grace_ms_;
    std::atomic<bool> requested_{false};
    bool deadline_initialized_ = false;
    int64_t deadline_ms_ = 0;
};

#endif
