#include "RpcPendingCall.h"
#include "CircuitBreaker.h"
#include "ConnContext.h"
#include "KrpcConnectPool.h"
#include "Krpccontroller.h"
#include "TimeoutWheel.h"

void ApplyRpcFinish(const std::shared_ptr<RpcPendingCall> &call)
{
    if (!call)
    {
        return;
    }

    TimeoutWheel::Unregister(call.get());

    auto ctx = GetConnContext(call->conn);
    if (ctx)
    {
        const auto erased = ctx->pending.erase(call->request_id);
        if (erased > 0)
        {
            ctx->inflight.fetch_sub(1, std::memory_order_relaxed);
            if (ctx->inflight.load(std::memory_order_relaxed) < 0)
            {
                ctx->inflight.store(0, std::memory_order_relaxed);
            }
        }
    }

    if (call->ok)
    {
        CircuitBreaker::Instance().RecordSuccess(call->node);
        if (ctx && ctx->zc_busy.load(std::memory_order_acquire) != 0)
        {
            // 内核还在用发送缓冲，这条连接先不进入可发队列。
        }
        else
        {
            KrpcConnectPool::GetInstance().MaybeMakeAvailable(call->conn);
        }
    }
    else
    {
        CircuitBreaker::Instance().RecordFailure(call->node);
        if (call->close_on_finish)
        {
            if (ctx)
            {
                ctx->inflight_zc.clear();
                ctx->zc_busy.store(0, std::memory_order_release);
            }
            KrpcConnectPool::GetInstance().CloseConnection(call->conn);
        }
        else
        {
            KrpcConnectPool::GetInstance().MaybeMakeAvailable(call->conn);
        }
    }

    // 同步调用只通过 pending->ok/err 把结果带回调用线程，避免跨线程写 controller。
    if (call->done != nullptr)
    {
        if (!call->ok && call->controller)
        {
            SetRpcFailed(call->controller, call->error_code, call->err);
        }
        call->NotifyDone();
    }
}

void FinishRpcCall(const std::shared_ptr<RpcPendingCall> &call,
                   bool ok,
                   const std::string &err,
                   bool close_conn,
                   int error_code)
{
    if (!call || !call->TryComplete(ok, err, close_conn, error_code))
    {
        return;
    }
    ApplyRpcFinish(call);
}
