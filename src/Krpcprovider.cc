#include "Krpcprovider.h"
#include "ConnContext.h"
#include "Krpcapplication.h"
#include "Krpccontroller.h"
#include "Krpcheader.pb.h"
#include "KrpcLogger.h"
#include "RpcCodec.h"
#include "RpcError.h"
#include "RpcObjectPool.h"
#include "RpcPendingCall.h"
#include "RpcMetrics.h"
#include "TcpSockUtil.h"
#include "ZeroCopySend.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
std::atomic<KrpcProvider *> g_provider{nullptr};

void HandleStopSignal(int)
{
    auto *p = g_provider.load(std::memory_order_acquire);
    if (p)
    {
        p->RequestStop();
    }
}
} // namespace

KrpcProvider::KrpcProvider()
    : m_shutdown(KrpcApplication::ServerShutdownGraceMs())
{
}

void KrpcProvider::NotifyService(google::protobuf::Service *service)
{
    ServiceInfo service_info;
    const google::protobuf::ServiceDescriptor *psd = service->GetDescriptor();
    std::string service_name = psd->name();
    int method_count = psd->method_count();

    std::cout << "service_name=" << service_name << std::endl;

    for (int i = 0; i < method_count; ++i)
    {
        const google::protobuf::MethodDescriptor *pmd = psd->method(i);
        std::string method_name = pmd->name();
        std::cout << "method_name=" << method_name << std::endl;
        service_info.method_map.emplace(method_name, pmd);
    }
    service_info.service = service;
    service_map.emplace(service_name, service_info);
}

void KrpcProvider::RequestStop()
{
    m_shutdown.Request();
}

void KrpcProvider::Run()
{
    std::string ip = KrpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    int port = atoi(KrpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());

    muduo::net::InetAddress address(ip, port);
    m_server = std::make_shared<muduo::net::TcpServer>(&event_loop, address, "KrpcProvider");

    m_server->setConnectionCallback(std::bind(&KrpcProvider::OnConnection, this, std::placeholders::_1));
    m_server->setMessageCallback(
        std::bind(&KrpcProvider::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    m_server->setThreadInitCallback([](muduo::net::EventLoop *loop) {
        loop->runEvery(0.01, []() { ZeroCopySend::DrainLocal(); });
    });

    const int cores = KrpcApplication::CpuCores();
    m_server->setThreadNum(cores);
    m_thread_pool.start(cores);

    m_zk.Start();
    for (auto &sp : service_map)
    {
        std::string service_path = "/" + sp.first;
        m_zk.Create(service_path.c_str(), nullptr, 0);

        std::string ip_port = ip + ":" + std::to_string(port);
        std::string instance_path = service_path + "/" + ip_port;
        m_zk.Create(instance_path.c_str(), ip_port.c_str(), static_cast<int>(ip_port.length()), ZOO_EPHEMERAL);
    }

    g_provider.store(this, std::memory_order_release);
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);
    event_loop.runEvery(0.05, [this]() {
        if (m_shutdown.Requested() && !m_drain_started)
        {
            m_drain_started = true;
            m_zk.Stop();
            LOG(INFO) << "RpcProvider draining, pending_jobs=" << m_work.Total();
        }
        if (m_shutdown.ShouldStop(m_work.Total(), RpcNowMs()))
        {
            LOG(INFO) << "RpcProvider drain finished, pending_jobs=" << m_work.Total();
            event_loop.quit();
        }
    });

    std::cout << "RpcProvider start service at ip:" << ip << " port:" << port << std::endl;

    m_server->start();
    event_loop.loop();

    g_provider.store(nullptr, std::memory_order_release);
    if (!m_drain_started)
    {
        m_zk.Stop();
    }
    m_thread_pool.stop();
    RpcMetrics::Instance().Dump("provider");
    std::cout << "RpcProvider stopped" << std::endl;
}

void KrpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        if (m_shutdown.Requested())
        {
            conn->forceClose();
            return;
        }
        conn->setTcpNoDelay(true);
        auto ctx = EnsureConnContext(conn);
        ctx->fd = LookupTcpFd(conn);
        if (ctx->fd >= 0)
        {
            SetTcpKeepalive(ctx->fd, KrpcApplication::TcpKeepaliveIdleS());
            ctx->zc_sock_ok = KrpcApplication::EnableZeroCopy() && EnableSockZeroCopy(ctx->fd);
        }
        return;
    }
}

void KrpcProvider::SendFrame(const muduo::net::TcpConnectionPtr &conn, std::string frame)
{
    if (!conn)
    {
        return;
    }
    m_work.BeginSend();
    auto send_fn = [this, conn, frame = std::move(frame)]() {
        if (conn->connected())
        {
            auto buf = std::make_shared<std::vector<char>>(frame.begin(), frame.end());
            if (!ZeroCopySend::TrySend(conn, buf))
            {
                conn->send(buf->data(), static_cast<int>(buf->size()));
            }
        }
        m_work.EndSend();
    };
    if (conn->getLoop()->isInLoopThread())
    {
        send_fn();
    }
    else
    {
        conn->getLoop()->runInLoop(send_fn);
    }
}

void KrpcProvider::SendError(const muduo::net::TcpConnectionPtr &conn,
                             uint64_t request_id,
                             int code,
                             const std::string &msg)
{
    Krpc::RpcMeta meta;
    meta.set_request_id(request_id);
    meta.set_error_code(code);
    meta.set_error_msg(msg);
    std::string header;
    std::string frame;
    if (!meta.SerializeToString(&header) ||
        !EncodeRpcFrame(header, "", KrpcApplication::RpcMaxBodyBytes(), &frame))
    {
        conn->forceClose();
        return;
    }
    SendFrame(conn, std::move(frame));
}

static void FinishServerRpc(uint64_t request_id,
                            const std::string &service,
                            const std::string &method,
                            int code,
                            int64_t start_us)
{
    const uint64_t latency_us =
        static_cast<uint64_t>(std::max<int64_t>(0, RpcNowUs() - start_us));
    RpcMetrics::Instance().RecordFinished(code, latency_us);
    MaybeRpcAccessLog("server", request_id, service, method, "", code, latency_us);
}

void KrpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                             muduo::net::Buffer *buffer,
                             muduo::Timestamp receive_time)
{
    (void)receive_time;
    const uint32_t max_bytes = KrpcApplication::RpcMaxBodyBytes();
    const int max_pending = KrpcApplication::ServerMaxPending();

    while (true)
    {
        std::string header_bytes;
        std::string args;
        const RpcDecodeStatus st = DecodeRpcFrame(buffer, max_bytes, &header_bytes, &args);
        if (st == RpcDecodeStatus::NeedMore)
        {
            return;
        }
        if (st == RpcDecodeStatus::Corrupt)
        {
            LOG(ERROR) << "rpc frame corrupt, close connection";
            conn->forceClose();
            return;
        }

        Krpc::RpcHeader krpcHeader;
        if (!krpcHeader.ParseFromString(header_bytes))
        {
            LOG(ERROR) << "header parse error, close connection";
            conn->forceClose();
            return;
        }

        const uint64_t request_id = krpcHeader.request_id();
        const std::string service_name = krpcHeader.service_name();
        const std::string method_name = krpcHeader.method_name();
        const int64_t start_us = RpcNowUs();
        RpcMetrics::Instance().RecordStarted();
        if (m_shutdown.Requested())
        {
            SendError(conn, request_id, kRpcOverloaded, "server shutting down");
            FinishServerRpc(request_id, service_name, method_name, kRpcOverloaded, start_us);
            continue;
        }
        if (krpcHeader.args_size() != static_cast<google::protobuf::uint32>(args.size()))
        {
            SendError(conn, request_id, kRpcBadRequest, "args_size mismatch");
            FinishServerRpc(request_id, service_name, method_name, kRpcBadRequest, start_us);
            continue;
        }

        auto it = service_map.find(service_name);
        if (it == service_map.end())
        {
            SendError(conn, request_id, kRpcNoService, service_name + " is not exist");
            FinishServerRpc(request_id, service_name, method_name, kRpcNoService, start_us);
            continue;
        }
        auto mit = it->second.method_map.find(method_name);
        if (mit == it->second.method_map.end())
        {
            SendError(conn, request_id, kRpcNoMethod, service_name + "." + method_name + " is not exist");
            FinishServerRpc(request_id, service_name, method_name, kRpcNoMethod, start_us);
            continue;
        }

        if (!m_work.TryAcquireJob(max_pending))
        {
            SendError(conn, request_id, kRpcOverloaded, "server overloaded");
            FinishServerRpc(request_id, service_name, method_name, kRpcOverloaded, start_us);
            continue;
        }

        google::protobuf::Service *service = it->second.service;
        const google::protobuf::MethodDescriptor *method = mit->second;

        m_thread_pool.run([this, conn, service, method, args = std::move(args), request_id,
                           service_name, method_name, start_us]() {
            google::protobuf::Message *request = RpcObjectPool::AcquireRequest(service, method);
            google::protobuf::Message *response = RpcObjectPool::AcquireResponse(service, method);
            if (!request->ParseFromArray(args.data(), static_cast<int>(args.size())))
            {
                SendError(conn, request_id, kRpcBadRequest, "Parse request error");
                FinishServerRpc(request_id, service_name, method_name, kRpcBadRequest, start_us);
                RpcObjectPool::Release(request);
                RpcObjectPool::Release(response);
                m_work.ReleaseJob();
                return;
            }
            auto *ctrl = new Krpccontroller();
            KrpcClosure *done = RpcObjectPool::AcquireClosure();
            done->Reset([this, conn, response, request, request_id, ctrl, service_name, method_name,
                         start_us]() {
                this->SendRpcResponse(conn, response, request, request_id, ctrl);
                const int code = ctrl->Failed() ? ctrl->ErrorCode() : kRpcOk;
                FinishServerRpc(request_id, service_name, method_name, code, start_us);
                delete ctrl;
                m_work.ReleaseJob();
            });
            service->CallMethod(method, ctrl, request, response, done);
        });
    }
}

void KrpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn,
                                   google::protobuf::Message *response,
                                   google::protobuf::Message *request,
                                   uint64_t request_id,
                                   Krpccontroller *controller)
{
    if (controller && controller->Failed())
    {
        SendError(conn, request_id, controller->ErrorCode(), controller->ErrorText());
        RpcObjectPool::Release(response);
        RpcObjectPool::Release(request);
        return;
    }

    Krpc::RpcMeta meta;
    meta.set_request_id(request_id);
    meta.set_error_code(kRpcOk);
    std::string header;
    std::string body;
    std::string frame;
    if (!meta.SerializeToString(&header) || !response->SerializeToString(&body) ||
        !EncodeRpcFrame(header, body, KrpcApplication::RpcMaxBodyBytes(), &frame))
    {
        LOG(ERROR) << "serialize response error!";
        SendError(conn, request_id, kRpcInternal, "serialize response error");
    }
    else
    {
        SendFrame(conn, std::move(frame));
    }

    RpcObjectPool::Release(response);
    RpcObjectPool::Release(request);
}

KrpcProvider::~KrpcProvider()
{
    RequestStop();
    event_loop.quit();
    for (auto &sp : service_map)
    {
        if (sp.second.service != nullptr)
        {
            delete sp.second.service;
            sp.second.service = nullptr;
        }
    }
}
