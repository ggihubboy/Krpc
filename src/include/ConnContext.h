#ifndef KRPC_CONN_CONTEXT_H
#define KRPC_CONN_CONTEXT_H

#include "RpcPendingCall.h"

#include <atomic>
#include <boost/any.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

struct ZcSendRec
{
    uint32_t first_seq = 0;
    uint32_t last_seq = 0;
    std::shared_ptr<std::vector<char>> buf;
};

// 挂在 TcpConnection::setContext 上。pending 只在该连接所属 EventLoop 上读写。
struct ConnContext
{
    int fd = -1;
    int64_t last_idle_ms = 0;
    bool zc_sock_ok = false;
    bool connect_accounted = false;
    uint32_t zc_next_seq = 0;
    std::atomic<int> inflight{0};
    std::atomic<bool> in_available{false};
    std::atomic<int> zc_busy{0};
    std::unordered_map<uint64_t, std::shared_ptr<RpcPendingCall>> pending;
    std::deque<ZcSendRec> inflight_zc;
};

inline std::shared_ptr<ConnContext> GetConnContext(const muduo::net::TcpConnectionPtr &conn)
{
    if (!conn || conn->getContext().empty())
    {
        return nullptr;
    }
    try
    {
        return boost::any_cast<std::shared_ptr<ConnContext>>(conn->getContext());
    }
    catch (const boost::bad_any_cast &)
    {
        return nullptr;
    }
}

inline std::shared_ptr<ConnContext> EnsureConnContext(const muduo::net::TcpConnectionPtr &conn)
{
    auto ctx = GetConnContext(conn);
    if (ctx)
    {
        return ctx;
    }
    ctx = std::make_shared<ConnContext>();
    conn->setContext(ctx);
    return ctx;
}

#endif
