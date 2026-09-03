#ifndef KRPC_RETRY_ATTEMPT_STATE_H
#define KRPC_RETRY_ATTEMPT_STATE_H

#include "RetryPolicy.h"

#include <atomic>

// Coordinates one safe pre-send retry and a single final completion.
class RetryAttemptState
{
public:
    bool TryStartRetry(int error_code, bool request_submitted)
    {
        if (request_submitted || !IsSafePreSendRetry(error_code))
        {
            return false;
        }
        int expected = 0;
        return retries_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                std::memory_order_relaxed);
    }

    bool TryFinish()
    {
        bool expected = false;
        return finished_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed);
    }

private:
    std::atomic<int> retries_{0};
    std::atomic<bool> finished_{false};
};

#endif
