#ifndef KRPC_PENDING_CALL_H
#define KRPC_PENDING_CALL_H

#include "RpcError.h"

#include <google/protobuf/service.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

inline int64_t RpcNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 一条连接上可以同时挂多个未完成 RPC，用 request_id 对应回包。
struct RpcPendingCall
{
    google::protobuf::RpcController *controller = nullptr;
    google::protobuf::Message *response = nullptr;
    google::protobuf::Closure *done = nullptr;
    muduo::net::EventLoop *loop = nullptr;
    muduo::net::TcpConnectionPtr conn;
    std::string node;
    uint64_t request_id = 0;
    int64_t deadline_ms = 0;
    bool close_on_finish = false;

    std::atomic<bool> completed{false};
    bool ok = false;
    int error_code = kRpcOk;
    std::string err;
    std::mutex mu;
    std::condition_variable cv;
    bool result_published = false;

    // 先写入 ok/err 再发布。只有赢家返回 true。
    bool TryComplete(bool success,
                     const std::string &error,
                     bool close_conn = false,
                     int code = kRpcInternal)
    {
        bool expected = false;
        if (!completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu);
        ok = success;
        error_code = success ? kRpcOk : code;
        err = error;
        close_on_finish = close_conn;
        result_published = true;
        cv.notify_one();
        return true;
    }

    bool Wait(int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (timeout_ms < 0)
        {
            timeout_ms = 0;
        }
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this]() { return result_published; });
    }

    void NotifyDone()
    {
        if (done != nullptr)
        {
            done->Run();
        }
    }
};

void FinishRpcCall(const std::shared_ptr<RpcPendingCall> &call,
                   bool ok,
                   const std::string &err,
                   bool close_conn = false,
                   int error_code = kRpcInternal);

void ApplyRpcFinish(const std::shared_ptr<RpcPendingCall> &call);

#endif
