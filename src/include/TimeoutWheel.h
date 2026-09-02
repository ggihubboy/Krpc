#ifndef KRPC_TIMEOUT_WHEEL_H
#define KRPC_TIMEOUT_WHEEL_H

#include "RpcPendingCall.h"

#include <memory>

// 每个客户端 EventLoop 一条「进行中」表，只在该 loop 线程增删。
// 用 10ms tick 扫描异步超时，避免每笔 RPC 都往 TimerQueue 插节点。
class TimeoutWheel
{
public:
    static void Register(const std::shared_ptr<RpcPendingCall> &call);
    static void Unregister(RpcPendingCall *call);
    static void Scan();
};

#endif
