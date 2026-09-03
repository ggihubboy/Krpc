#ifndef KRPC_PENDING_WORK_H
#define KRPC_PENDING_WORK_H

#include <atomic>

// Server-side work that must drain before process exit: business-thread jobs
// plus responses that have been handed to an EventLoop but not yet sent.
class PendingWork
{
public:
    bool TryAcquireJob(int limit)
    {
        if (limit <= 0)
        {
            return false;
        }
        int current = jobs_.load(std::memory_order_relaxed);
        while (current < limit)
        {
            if (jobs_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    void ReleaseJob()
    {
        jobs_.fetch_sub(1, std::memory_order_acq_rel);
    }

    void BeginSend()
    {
        sends_.fetch_add(1, std::memory_order_acq_rel);
    }

    void EndSend()
    {
        sends_.fetch_sub(1, std::memory_order_acq_rel);
    }

    int Jobs() const
    {
        return jobs_.load(std::memory_order_acquire);
    }

    int Total() const
    {
        return jobs_.load(std::memory_order_acquire) + sends_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int> jobs_{0};
    std::atomic<int> sends_{0};
};

#endif
