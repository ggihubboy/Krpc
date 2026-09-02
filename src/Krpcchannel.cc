#include "Krpcchannel.h"
#include "CircuitBreaker.h"
#include "ConnContext.h"
#include "KrpcConnectPool.h"
#include "Krpcapplication.h"
#include "Krpccontroller.h"
#include "Krpcheader.pb.h"
#include "RpcCodec.h"
#include "RpcError.h"
#include "RpcPendingCall.h"
#include "RetryPolicy.h"
#include "RpcMetrics.h"
#include "ServiceDiscovery.h"
#include "TimeoutWheel.h"
#include "ZeroCopySend.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>
#include <vector>

static std::atomic<uint64_t> g_req_id{1};

static void FailDone(google::protobuf::RpcController *controller,
                     google::protobuf::Closure *done,
                     int error_code,
                     const std::string &err,
                     bool notify_done = true)
{
    SetRpcFailed(controller, error_code, err);
    if (done && notify_done)
    {
        done->Run();
    }
}

static void FailImmediate(google::protobuf::RpcController *controller,
                          google::protobuf::Closure *done,
                          int error_code,
                          const std::string &err,
                          int64_t start_us,
                          uint64_t request_id,
                          const std::string &service,
                          const std::string &method,
                          const std::string &node = std::string())
{
    const uint64_t latency_us =
        static_cast<uint64_t>(std::max<int64_t>(0, RpcNowUs() - start_us));
    RpcMetrics::Instance().RecordFinished(error_code, latency_us);
    MaybeRpcAccessLog("client", request_id, service, method, node, error_code, latency_us);
    FailDone(controller, done, error_code, err);
}

static bool ParseNode(const std::string &ip_port, std::string *ip, uint16_t *port)
{
    const auto pos = ip_port.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= ip_port.size())
    {
        return false;
    }
    *ip = ip_port.substr(0, pos);
    *port = static_cast<uint16_t>(std::atoi(ip_port.c_str() + pos + 1));
    return !ip->empty() && *port != 0;
}

static bool BuildPayload(const google::protobuf::MethodDescriptor *method,
                         const google::protobuf::Message *request,
                         uint64_t request_id,
                         std::string *out)
{
    thread_local std::string args_str;
    thread_local std::string header_str;

    args_str.clear();
    if (!request->SerializeToString(&args_str))
    {
        return false;
    }

    Krpc::RpcHeader header;
    header.set_service_name(method->service()->name());
    header.set_method_name(method->name());
    header.set_args_size(static_cast<google::protobuf::uint32>(args_str.size()));
    header.set_request_id(request_id);
    header_str.clear();
    if (!header.SerializeToString(&header_str))
    {
        return false;
    }
    return EncodeRpcFrame(header_str, args_str, KrpcApplication::RpcMaxBodyBytes(), out);
}

bool KrpcChannel::IssueOnce(const std::string &node,
                            const std::string &payload,
                            uint64_t request_id,
                            google::protobuf::RpcController *controller,
                            google::protobuf::Message *response,
                            google::protobuf::Closure *done,
                            int timeout_ms,
                            bool notify_done_on_immediate_failure,
                            bool *pre_send_failure,
                            const std::string &service,
                            const std::string &method,
                            int64_t start_us)
{
    if (pre_send_failure != nullptr)
    {
        *pre_send_failure = false;
    }
    std::string ip;
    uint16_t port = 0;
    if (!ParseNode(node, &ip, &port))
    {
        if (pre_send_failure != nullptr)
        {
            *pre_send_failure = true;
        }
        FailDone(controller, done, kRpcBadRequest, "invalid node address",
                 notify_done_on_immediate_failure);
        return false;
    }

    const int borrow_timeout = std::min(timeout_ms, 2000);
    auto conn = KrpcConnectPool::GetInstance().BorrowConnection(ip, port, borrow_timeout);
    if (!conn)
    {
        if (pre_send_failure != nullptr)
        {
            *pre_send_failure = true;
        }
        FailDone(controller, done, kRpcConnectFail, "Borrow connection from pool failed!",
                 notify_done_on_immediate_failure);
        CircuitBreaker::Instance().RecordFailure(node);
        return false;
    }

    auto pending = std::make_shared<RpcPendingCall>();
    pending->controller = controller;
    pending->response = response;
    pending->done = done;
    pending->node = node;
    pending->service = service;
    pending->method = method;
    pending->conn = conn;
    pending->loop = conn->getLoop();
    pending->request_id = request_id;
    pending->deadline_ms = RpcNowMs() + timeout_ms;
    pending->start_us = start_us;

    pending->loop->runInLoop([conn, pending, payload]() {
        if (pending->completed.load(std::memory_order_acquire))
        {
            KrpcConnectPool::GetInstance().MaybeMakeAvailable(conn);
            return;
        }
        if (!conn->connected())
        {
            FinishRpcCall(pending, false, "connection closed before send", true, kRpcConnectFail);
            return;
        }
        auto ctx = EnsureConnContext(conn);
        ctx->pending[pending->request_id] = pending;
        ctx->inflight.fetch_add(1, std::memory_order_relaxed);
        if (pending->done != nullptr)
        {
            TimeoutWheel::Register(pending);
        }
        bool sent = false;
        if (ZeroCopySend::ShouldUse(payload.size(), conn))
        {
            auto buf = std::make_shared<std::vector<char>>(payload.begin(), payload.end());
            sent = ZeroCopySend::TrySend(conn, buf);
        }
        if (!sent)
        {
            conn->send(payload.data(), static_cast<int>(payload.size()));
        }
        KrpcConnectPool::GetInstance().MaybeMakeAvailable(conn);
    });

    if (done == nullptr)
    {
        if (!pending->Wait(timeout_ms))
        {
            if (pending->TryComplete(false, "rpc timeout", false, kRpcTimeout))
            {
                SetRpcFailed(controller, kRpcTimeout, "rpc timeout");
                pending->loop->queueInLoop([pending]() { ApplyRpcFinish(pending); });
                return false;
            }
        }
        if (!pending->ok)
        {
            SetRpcFailed(controller, pending->error_code, pending->err);
            return false;
        }
        return true;
    }
    return true;
}

void KrpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                             ::google::protobuf::RpcController *controller,
                             const ::google::protobuf::Message *request,
                             ::google::protobuf::Message *response,
                             ::google::protobuf::Closure *done)
{
    static std::once_flag init_flag;
    std::call_once(init_flag, []() { ServiceDiscovery::GetInstance().Init(); });

    const std::string service_name = method->service()->name();
    const std::string method_name = method->name();
    const uint64_t req_id = g_req_id.fetch_add(1, std::memory_order_relaxed);
    const int timeout_ms = KrpcApplication::RpcTimeoutMs();
    const int64_t start_us = RpcNowUs();
    const int64_t overall_deadline_ms = RpcNowMs() + timeout_ms;
    RpcMetrics::Instance().RecordStarted();

    std::string payload;
    if (!BuildPayload(method, request, req_id, &payload))
    {
        FailImmediate(controller, done, kRpcBadRequest, "Serialize request fail",
                      start_us, req_id, service_name, method_name);
        return;
    }

    auto pick_node = [&](const std::string &exclude) {
        return ServiceDiscovery::GetInstance().GetTargetNode(service_name, std::to_string(req_id), exclude);
    };

    std::string node = pick_node("");
    if (node.empty())
    {
        FailImmediate(controller, done, kRpcNoService, "Hash ring returned empty node!",
                      start_us, req_id, service_name, method_name);
        return;
    }

    if (!CircuitBreaker::Instance().AllowRequest(node))
    {
        const std::string alt = pick_node(node);
        if (alt.empty() || !CircuitBreaker::Instance().AllowRequest(alt))
        {
            FailImmediate(controller, done, kRpcCircuitOpen, "circuit open",
                          start_us, req_id, service_name, method_name, node);
            return;
        }
        node = alt;
    }

    bool pre_send_failure = false;
    const bool ok = IssueOnce(node, payload, req_id, controller, response, done, timeout_ms,
                              false, &pre_send_failure, service_name, method_name, start_us);
    if (ok)
    {
        return;
    }

    const auto *krpc_controller = dynamic_cast<const Krpccontroller *>(controller);
    const int first_error = krpc_controller != nullptr ? krpc_controller->ErrorCode() : kRpcInternal;
    if (!pre_send_failure || !IsSafePreSendRetry(first_error))
    {
        if (pre_send_failure)
        {
            const uint64_t latency_us =
                static_cast<uint64_t>(std::max<int64_t>(0, RpcNowUs() - start_us));
            RpcMetrics::Instance().RecordFinished(first_error, latency_us);
            MaybeRpcAccessLog("client", req_id, service_name, method_name, node, first_error,
                              latency_us);
        }
        if (done != nullptr)
        {
            done->Run();
        }
        return;
    }

    const std::string alt = pick_node(node);
    if (alt.empty() || !CircuitBreaker::Instance().AllowRequest(alt))
    {
        FailImmediate(controller, done, first_error,
                      controller != nullptr ? controller->ErrorText() : "retry node unavailable",
                      start_us, req_id, service_name, method_name, node);
        return;
    }

    const int remaining_ms = static_cast<int>(overall_deadline_ms - RpcNowMs());
    if (remaining_ms <= 0)
    {
        FailImmediate(controller, done, kRpcTimeout, "rpc timeout",
                      start_us, req_id, service_name, method_name, node);
        return;
    }

    controller->Reset();
    const uint64_t retry_id = g_req_id.fetch_add(1, std::memory_order_relaxed);
    std::string retry_payload;
    if (!BuildPayload(method, request, retry_id, &retry_payload))
    {
        FailImmediate(controller, done, kRpcBadRequest, "Serialize request fail",
                      start_us, retry_id, service_name, method_name, alt);
        return;
    }

    bool retry_pre_send_failure = false;
    const bool retry_ok = IssueOnce(alt, retry_payload, retry_id, controller, response, done,
                                    remaining_ms, true, &retry_pre_send_failure, service_name,
                                    method_name, start_us);
    if (!retry_ok && retry_pre_send_failure)
    {
        const auto *retry_controller = dynamic_cast<const Krpccontroller *>(controller);
        const int retry_error =
            retry_controller != nullptr ? retry_controller->ErrorCode() : kRpcInternal;
        const uint64_t latency_us =
            static_cast<uint64_t>(std::max<int64_t>(0, RpcNowUs() - start_us));
        RpcMetrics::Instance().RecordFinished(retry_error, latency_us);
        MaybeRpcAccessLog("client", retry_id, service_name, method_name, alt, retry_error,
                          latency_us);
    }
}
