#include "KrpcConnectPool.h"
#include "ConnContext.h"
#include "KrpcClientIo.h"
#include "KrpcLogger.h"
#include "Krpcapplication.h"
#include "Krpcheader.pb.h"
#include "RpcCodec.h"
#include "RpcPendingCall.h"
#include "TcpSockUtil.h"
#include "ZeroCopySend.h"

#include <google/protobuf/message.h>
#include <algorithm>
#include <atomic>
#include <boost/any.hpp>
#include <chrono>
#include <future>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <vector>

KrpcConnectPool &KrpcConnectPool::GetInstance()
{
    static KrpcConnectPool instance;
    return instance;
}

ConnectionBucket *KrpcConnectPool::GetBucket(const std::string &ip, uint16_t port)
{
    thread_local std::string last_ip;
    thread_local uint16_t last_port = 0;
    thread_local ConnectionBucket *last_bucket = nullptr;
    if (last_bucket != nullptr && last_port == port && last_ip == ip)
    {
        return last_bucket;
    }

    const std::string key = ip + ":" + std::to_string(port);
    ConnectionBucket *bucket = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_global_mtx);
        auto it = m_pools.find(key);
        if (it == m_pools.end())
        {
            auto created = std::make_unique<ConnectionBucket>(kMaxConnPerNode);
            created->ip = ip;
            created->port = port;
            bucket = created.get();
            m_pools.emplace(key, std::move(created));
        }
        else
        {
            bucket = it->second.get();
        }
    }

    last_ip = ip;
    last_port = port;
    last_bucket = bucket;
    return bucket;
}

static void SetupNewConnection(const muduo::net::TcpConnectionPtr &conn)
{
    conn->setTcpNoDelay(true);
    auto ctx = EnsureConnContext(conn);
    ctx->fd = LookupTcpFd(conn);
    ctx->last_idle_ms = RpcNowMs();
    if (ctx->fd >= 0)
    {
        SetTcpKeepalive(ctx->fd, KrpcApplication::TcpKeepaliveIdleS());
        ctx->zc_sock_ok = KrpcApplication::EnableZeroCopy() && EnableSockZeroCopy(ctx->fd);
    }
}

void KrpcConnectPool::FailAllPending(const muduo::net::TcpConnectionPtr &conn, const std::string &err)
{
    auto ctx = GetConnContext(conn);
    if (!ctx || ctx->pending.empty())
    {
        return;
    }
    std::vector<std::shared_ptr<RpcPendingCall>> calls;
    calls.reserve(ctx->pending.size());
    for (auto &pair : ctx->pending)
    {
        calls.push_back(pair.second);
    }
    ctx->pending.clear();
    ctx->inflight.store(0, std::memory_order_relaxed);
    for (auto &call : calls)
    {
        FinishRpcCall(call, false, err, false);
    }
}

void KrpcConnectPool::OnConnection(ConnectionBucket *bucket, const muduo::net::TcpConnectionPtr &conn)
{
    auto ctx = EnsureConnContext(conn);
    if (!ctx->connect_accounted)
    {
        ctx->connect_accounted = true;
        bucket->connecting.fetch_sub(1, std::memory_order_relaxed);
        if (bucket->connecting.load(std::memory_order_relaxed) < 0)
        {
            bucket->connecting.store(0, std::memory_order_relaxed);
        }
    }

    if (conn->connected())
    {
        SetupNewConnection(conn);
        bucket->ready.fetch_add(1, std::memory_order_relaxed);
        GetInstance().MaybeMakeAvailable(conn);
        return;
    }

    bucket->ready.fetch_sub(1, std::memory_order_relaxed);
    if (bucket->ready.load(std::memory_order_relaxed) < 0)
    {
        bucket->ready.store(0, std::memory_order_relaxed);
    }
    FailAllPending(conn, "connection closed");
}

void KrpcConnectPool::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                               muduo::net::Buffer *buffer,
                               muduo::Timestamp receive_time)
{
    (void)receive_time;
    const uint32_t max_bytes = KrpcApplication::RpcMaxBodyBytes();
    while (true)
    {
        std::string header_bytes;
        std::string payload;
        const RpcDecodeStatus st = DecodeRpcFrame(buffer, max_bytes, &header_bytes, &payload);
        if (st == RpcDecodeStatus::NeedMore)
        {
            return;
        }
        if (st == RpcDecodeStatus::Corrupt)
        {
            FailAllPending(conn, "rpc frame corrupt");
            conn->forceClose();
            return;
        }

        Krpc::RpcMeta meta;
        if (!meta.ParseFromString(header_bytes))
        {
            FailAllPending(conn, "rpc meta parse error");
            conn->forceClose();
            return;
        }

        auto ctx = GetConnContext(conn);
        std::shared_ptr<RpcPendingCall> call;
        if (ctx)
        {
            auto it = ctx->pending.find(meta.request_id());
            if (it != ctx->pending.end())
            {
                call = it->second;
            }
        }
        if (!call || call->completed.load(std::memory_order_acquire))
        {
            continue;
        }

        if (meta.error_code() != 0)
        {
            FinishRpcCall(call, false, meta.error_msg().empty() ? "rpc error" : meta.error_msg(), false);
            continue;
        }
        if (call->response == nullptr)
        {
            FinishRpcCall(call, false, "null response", false);
            continue;
        }
        const bool ok = call->response->ParseFromString(payload);
        FinishRpcCall(call, ok, ok ? "" : "Parse response error", !ok);
    }
}

void KrpcConnectPool::StartConnect(ConnectionBucket *bucket)
{
    if (bucket->connecting.load(std::memory_order_relaxed) >= kMaxConnecting)
    {
        return;
    }
    if (bucket->ready.load(std::memory_order_relaxed) >= kMaxConnPerNode)
    {
        return;
    }
    bucket->connecting.fetch_add(1, std::memory_order_relaxed);

    muduo::net::EventLoop *loop = KrpcClientIo::Instance().NextLoop();
    loop->runInLoop([bucket, loop]() {
        {
            std::lock_guard<std::mutex> lock(bucket->clients_mtx);
            if (bucket->clients.size() >= static_cast<size_t>(kMaxConnPerNode))
            {
                bucket->connecting.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }
        muduo::net::InetAddress addr(bucket->ip, bucket->port);
        static std::atomic<int> conn_id{0};
        const std::string name = "rpc-" + bucket->ip + "-" + std::to_string(bucket->port) + "-" +
                                 std::to_string(conn_id.fetch_add(1, std::memory_order_relaxed));
        auto client = std::make_unique<muduo::net::TcpClient>(loop, addr, name);
        client->setConnectionCallback([bucket](const muduo::net::TcpConnectionPtr &conn) {
            OnConnection(bucket, conn);
        });
        client->setMessageCallback(&KrpcConnectPool::OnMessage);
        muduo::net::TcpClient *raw = client.get();
        {
            std::lock_guard<std::mutex> lock(bucket->clients_mtx);
            bucket->clients.push_back(std::move(client));
        }
        raw->connect();
    });
}

KrpcConnectPool::~KrpcConnectPool()
{
    for (auto &pair : m_pools)
    {
        std::lock_guard<std::mutex> lock(pair.second->clients_mtx);
        for (auto &client : pair.second->clients)
        {
            client.release();
        }
        pair.second->clients.clear();
    }
}

void KrpcConnectPool::Shutdown()
{
    if (!KrpcClientIo::Instance().Started())
    {
        return;
    }

    std::vector<ConnectionBucket *> buckets;
    {
        std::lock_guard<std::mutex> lock(m_global_mtx);
        buckets.reserve(m_pools.size());
        for (auto &pair : m_pools)
        {
            buckets.push_back(pair.second.get());
        }
    }

    LOG(INFO) << "zerocopy stats send=" << ZeroCopySend::SendCount()
              << " copied=" << ZeroCopySend::CopiedCount()
              << " complete=" << ZeroCopySend::CompleteCount();

    for (ConnectionBucket *bucket : buckets)
    {
        std::vector<std::unique_ptr<muduo::net::TcpClient>> local;
        {
            std::lock_guard<std::mutex> lock(bucket->clients_mtx);
            local.swap(bucket->clients);
        }
        for (auto &client : local)
        {
            if (!client)
            {
                continue;
            }
            muduo::net::EventLoop *loop = client->getLoop();
            std::promise<void> done;
            auto fut = done.get_future();
            loop->runInLoop([&client, &done]() {
                client->disconnect();
                client.reset();
                done.set_value();
            });
            fut.wait();
        }
    }
}

void KrpcConnectPool::PushAvailable(ConnectionBucket *bucket, const muduo::net::TcpConnectionPtr &conn)
{
    if (!bucket->available->push(conn))
    {
        auto ctx = GetConnContext(conn);
        if (ctx)
        {
            ctx->in_available.store(false, std::memory_order_relaxed);
        }
        if (ctx && ctx->inflight.load(std::memory_order_relaxed) == 0)
        {
            conn->shutdown();
        }
        return;
    }
    bucket->wait_cv.notify_one();
}

void KrpcConnectPool::MaybeMakeAvailable(const muduo::net::TcpConnectionPtr &conn)
{
    if (!conn || !conn->connected())
    {
        return;
    }
    auto ctx = GetConnContext(conn);
    if (!ctx)
    {
        return;
    }
    if (ctx->inflight.load(std::memory_order_relaxed) >= KrpcApplication::MaxInflightPerConn())
    {
        return;
    }
    if (ctx->zc_busy.load(std::memory_order_acquire) != 0)
    {
        return;
    }
    bool expected = false;
    if (!ctx->in_available.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    ctx->last_idle_ms = RpcNowMs();
    ConnectionBucket *bucket = GetBucket(conn->peerAddress().toIp(), conn->peerAddress().toPort());
    PushAvailable(bucket, conn);
}

void KrpcConnectPool::CloseConnection(const muduo::net::TcpConnectionPtr &conn)
{
    if (!conn)
    {
        return;
    }
    FailAllPending(conn, "connection closed");
    conn->forceClose();
}

muduo::net::TcpConnectionPtr KrpcConnectPool::BorrowConnection(const std::string &ip, uint16_t port, int timeout_ms)
{
    ConnectionBucket *bucket = GetBucket(ip, port);
    const int evict_ms = KrpcApplication::ConnIdleEvictMs();
    const int max_inflight = KrpcApplication::MaxInflightPerConn();

    auto pop_ready = [bucket, evict_ms, max_inflight]() -> muduo::net::TcpConnectionPtr {
        muduo::net::TcpConnectionPtr conn;
        while (bucket->available->pop(conn))
        {
            auto ctx = GetConnContext(conn);
            if (ctx)
            {
                ctx->in_available.store(false, std::memory_order_relaxed);
            }
            if (!conn || !conn->connected())
            {
                continue;
            }
            if (ctx && evict_ms > 0 && ctx->inflight.load(std::memory_order_relaxed) == 0 &&
                (RpcNowMs() - ctx->last_idle_ms) > evict_ms)
            {
                conn->forceClose();
                continue;
            }
            if (ctx && ctx->inflight.load(std::memory_order_relaxed) >= max_inflight)
            {
                continue;
            }
            if (ctx && ctx->zc_busy.load(std::memory_order_acquire) != 0)
            {
                continue;
            }
            return conn;
        }
        return muduo::net::TcpConnectionPtr();
    };

    if (auto conn = pop_ready())
    {
        return conn;
    }

    StartConnect(bucket);
    if (timeout_ms <= 0)
    {
        return pop_ready();
    }

    muduo::net::TcpConnectionPtr conn;
    std::unique_lock<std::mutex> lock(bucket->wait_mtx);
    bucket->wait_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
        conn = pop_ready();
        return static_cast<bool>(conn);
    });
    return conn;
}

void KrpcConnectPool::WarmUp(const std::string &ip, uint16_t port, int count)
{
    ConnectionBucket *bucket = GetBucket(ip, port);
    const int target = std::min(count, kMaxConnPerNode);
    for (int i = 0; i < target; ++i)
    {
        StartConnect(bucket);
    }

    std::unique_lock<std::mutex> lock(bucket->wait_mtx);
    const bool ok = bucket->wait_cv.wait_for(lock, std::chrono::seconds(5), [&]() {
        return bucket->ready.load(std::memory_order_relaxed) >= target;
    });
    LOG(INFO) << "[ConnectPool] WarmUp " << (ok ? "complete" : "timeout") << ": "
              << bucket->ready.load(std::memory_order_relaxed) << " connections ready for " << ip << ":" << port;
}
