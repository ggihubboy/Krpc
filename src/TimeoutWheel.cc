#include "TimeoutWheel.h"

#include <memory>
#include <vector>

struct TimeoutWheelLocal
{
    std::vector<std::weak_ptr<RpcPendingCall>> pending;
};

static TimeoutWheelLocal &Local()
{
    thread_local TimeoutWheelLocal state;
    return state;
}

void TimeoutWheel::Register(const std::shared_ptr<RpcPendingCall> &call)
{
    if (call)
    {
        Local().pending.push_back(call);
    }
}

void TimeoutWheel::Unregister(RpcPendingCall *call)
{
    auto &v = Local().pending;
    for (size_t i = 0; i < v.size();)
    {
        auto sp = v[i].lock();
        if (!sp || sp.get() == call)
        {
            v[i] = std::move(v.back());
            v.pop_back();
            if (sp && sp.get() == call)
            {
                return;
            }
            continue;
        }
        ++i;
    }
}

void TimeoutWheel::Scan()
{
    auto &v = Local().pending;
    const int64_t now = RpcNowMs();
    for (size_t i = 0; i < v.size();)
    {
        auto sp = v[i].lock();
        if (!sp || sp->completed.load(std::memory_order_acquire))
        {
            v[i] = std::move(v.back());
            v.pop_back();
            continue;
        }
        if (now >= sp->deadline_ms && sp->TryComplete(false, "rpc timeout", false))
        {
            v[i] = std::move(v.back());
            v.pop_back();
            ApplyRpcFinish(sp);
            continue;
        }
        ++i;
    }
}
