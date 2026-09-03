#include "ZkHandleGuard.h"

ZkHandleGuard::ZkHandleGuard()
    : close_fn_([](zhandle_t *handle) { zookeeper_close(handle); })
{
}

ZkHandleGuard::ZkHandleGuard(CloseFn close_fn)
    : close_fn_(std::move(close_fn))
{
}

ZkHandleGuard::~ZkHandleGuard()
{
    Close();
}

void ZkHandleGuard::Replace(zhandle_t *handle)
{
    zhandle_t *old = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (handle_ == handle)
        {
            return;
        }
        old = handle_;
        handle_ = handle;
    }
    // WithHandle holds mu_ for the complete C API call, so acquiring the lock
    // above guarantees no call still uses old. Close outside the lock because
    // zookeeper_close may wait for callbacks which call back into this guard.
    if (old != nullptr)
    {
        close_fn_(old);
    }
}

void ZkHandleGuard::Close()
{
    Replace(nullptr);
}
