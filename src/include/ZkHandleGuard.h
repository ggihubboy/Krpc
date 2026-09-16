#ifndef KRPC_ZK_HANDLE_GUARD_H
#define KRPC_ZK_HANDLE_GUARD_H

#include "ZkCApi.h"

#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

// ZooKeeper allows calls from multiple threads, but closing/replacing a handle
// while another thread is using it is unsafe. This guard makes handle lifetime
// changes mutually exclusive with every C API call.
class ZkHandleGuard
{
public:
    using CloseFn = std::function<void(zhandle_t *)>;

    ZkHandleGuard();
    explicit ZkHandleGuard(CloseFn close_fn);
    ~ZkHandleGuard();

    ZkHandleGuard(const ZkHandleGuard &) = delete;
    ZkHandleGuard &operator=(const ZkHandleGuard &) = delete;

    template <typename Fn>
    auto WithHandle(Fn &&fn)
        -> std::optional<std::invoke_result_t<Fn, zhandle_t *>>
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (handle_ == nullptr)
        {
            return std::nullopt;
        }
        return std::invoke(std::forward<Fn>(fn), handle_);
    }

    void Replace(zhandle_t *handle);
    void Close();

private:
    std::mutex mu_;
    zhandle_t *handle_ = nullptr;
    CloseFn close_fn_;
};

#endif
