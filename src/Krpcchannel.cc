#include "Krpcchannel.h"
#include "CircuitBreaker.h"
#include "ConnContext.h"
#include "KrpcConnectPool.h"
#include "Krpcapplication.h"
#include "Krpcheader.pb.h"
#include "RpcCodec.h"
#include "RpcError.h"
#include "RpcPendingCall.h"
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
                     const std::string &err)
{
    if (controller)
    {
        controller->SetFailed(err);
    }
    if (done)
    {
        done->Run();
    }
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
                            int timeout_ms)
{
    std::string ip;
    uint16_t port = 0;
    if (!ParseNode(node, &ip, &port))
    {
        FailDone(controller, done, "invalid node address");
        return false;
    }

    const int borrow_timeout = std::min(timeout_ms, 2000);
    auto conn = KrpcConnectPool::GetInstance().BorrowConnection(ip, port, borrow_timeout);
    if (!conn)
    {
        FailDone(controller, done, "Borrow connection from pool failed!");
        CircuitBreaker::Instance().RecordFailure(node);
        return false;
    }

    auto pending = std::make_shared<RpcPendingCall>();
    pending->controller = controller;
    pending->response = response;
    pending->done = done;
    pending->node = node;
    pending->conn = conn;
    pending->loop = conn->getLoop();
    pending->request_id = request_id;
    pending->deadline_ms = RpcNowMs() + timeout_ms;

    pending->loop->runInLoop([conn, pending, payload]() {
        if (pending->completed.load(std::memory_order_acquire))
        {
            KrpcConnectPool::GetInstance().MaybeMakeAvailable(conn);
            return;
        }
        if (!conn->connected())
        {
            FinishRpcCall(pending, false, "connection closed before send", true);
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
            if (pending->TryComplete(false, "rpc timeout", false))
            {
                controller->SetFailed("rpc timeout");
                pending->loop->queueInLoop([pending]() { ApplyRpcFinish(pending); });
                return false;
            }
        }
        if (!pending->ok)
        {
            controller->SetFailed(pending->err);
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
    const uint64_t req_id = g_req_id.fetch_add(1, std::memory_order_relaxed);
    const int timeout_ms = KrpcApplication::RpcTimeoutMs();

    std::string payload;
    if (!BuildPayload(method, request, req_id, &payload))
    {
        FailDone(controller, done, "Serialize request fail");
        return;
    }

    auto pick_node = [&](const std::string &exclude) {
        return ServiceDiscovery::GetInstance().GetTargetNode(service_name, std::to_string(req_id), exclude);
    };

    std::string node = pick_node("");
    if (node.empty())
    {
        FailDone(controller, done, "Hash ring returned empty node!");
        return;
    }

    if (!CircuitBreaker::Instance().AllowRequest(node))
    {
        const std::string alt = pick_node(node);
        if (alt.empty() || !CircuitBreaker::Instance().AllowRequest(alt))
        {
            FailDone(controller, done, "circuit open");
            return;
        }
        node = alt;
    }

    const bool ok = IssueOnce(node, payload, req_id, controller, response, done, timeout_ms);
    if (done != nullptr || ok)
    {
        return;
    }

    const std::string alt = pick_node(node);
    if (alt.empty() || !CircuitBreaker::Instance().AllowRequest(alt))
    {
        return;
    }
    controller->Reset();
    const uint64_t retry_id = g_req_id.fetch_add(1, std::memory_order_relaxed);
    std::string retry_payload;
    if (!BuildPayload(method, request, retry_id, &retry_payload))
    {
        controller->SetFailed("Serialize request fail");
        return;
    }
    IssueOnce(alt, retry_payload, retry_id, controller, response, nullptr, timeout_ms);
}
