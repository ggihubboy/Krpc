#include "ZeroCopySend.h"
#include "ConnContext.h"
#include "KrpcConnectPool.h"
#include "Krpcapplication.h"
#include "KrpcLogger.h"

#include <linux/errqueue.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <vector>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

namespace
{
std::atomic<uint64_t> g_zc_send{0};
std::atomic<uint64_t> g_zc_copied{0};
std::atomic<uint64_t> g_zc_complete{0};

thread_local std::vector<std::weak_ptr<muduo::net::TcpConnection>> g_zc_conns;

void ReleaseCompleted(ConnContext *ctx, uint32_t lo, uint32_t hi, bool copied)
{
    while (!ctx->inflight_zc.empty())
    {
        const ZcSendRec &front = ctx->inflight_zc.front();
        // 序号落在 [lo, hi] 内才释放；内核可能一次通知一段连续范围。
        if (front.last_seq < lo)
        {
            ctx->inflight_zc.pop_front();
            continue;
        }
        if (front.first_seq > hi)
        {
            break;
        }
        ctx->inflight_zc.pop_front();
        g_zc_complete.fetch_add(1, std::memory_order_relaxed);
        if (copied)
        {
            g_zc_copied.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// 立刻抽空 ERRQUEUE。MSG_ZEROCOPY 完成通知会触发 EPOLLERR，
// Muduo 只读 SO_ERROR、不读 error queue，不抽空会在 EventLoop 里空转刷屏。
void DrainFd(ConnContext *ctx)
{
    if (ctx == nullptr || ctx->fd < 0 || ctx->inflight_zc.empty())
    {
        return;
    }
    while (true)
    {
        char cbuf[256];
        char dummy = 0;
        iovec iov{};
        iov.iov_base = &dummy;
        iov.iov_len = 1;
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        const ssize_t n = recvmsg(ctx->fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0)
        {
            break;
        }
        for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level != SOL_IP && cmsg->cmsg_level != SOL_IPV6 &&
                cmsg->cmsg_level != SOL_SOCKET)
            {
                continue;
            }
            auto *ee = reinterpret_cast<sock_extended_err *>(CMSG_DATA(cmsg));
            if (ee->ee_origin != SO_EE_ORIGIN_ZEROCOPY)
            {
                continue;
            }
            const bool copied = (ee->ee_code == SO_EE_CODE_ZEROCOPY_COPIED);
            ReleaseCompleted(ctx, ee->ee_info, ee->ee_data, copied);
        }
    }
}
} // namespace

bool ZeroCopySend::ShouldUse(size_t nbytes, const muduo::net::TcpConnectionPtr &conn)
{
    if (!KrpcApplication::EnableZeroCopy())
    {
        return false;
    }
    if (nbytes < static_cast<size_t>(KrpcApplication::ZeroCopyThreshold()))
    {
        return false;
    }
    if (!conn || !conn->connected())
    {
        return false;
    }
    if (conn->outputBuffer() && conn->outputBuffer()->readableBytes() > 0)
    {
        return false;
    }
    auto ctx = GetConnContext(conn);
    return ctx && ctx->zc_sock_ok && ctx->fd >= 0;
}

bool ZeroCopySend::TrySend(const muduo::net::TcpConnectionPtr &conn,
                           const std::shared_ptr<std::vector<char>> &buf)
{
    if (!buf || buf->empty() || !ShouldUse(buf->size(), conn))
    {
        return false;
    }

    auto ctx = GetConnContext(conn);
    const int fd = ctx->fd;
    const char *data = buf->data();
    size_t left = buf->size();
    size_t off = 0;
    uint32_t first_seq = ctx->zc_next_seq;
    uint32_t last_seq = first_seq;

    while (left > 0)
    {
        const ssize_t n = ::send(fd, data + off, left, MSG_NOSIGNAL | MSG_ZEROCOPY);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (off == 0)
                {
                    return false;
                }
                // 已经发出一部分，剩余走普通 send（output 仍为空时安全）。
                conn->send(data + off, static_cast<int>(left));
                break;
            }
            LOG(ERROR) << "MSG_ZEROCOPY send failed errno=" << errno;
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        last_seq = ctx->zc_next_seq;
        ctx->zc_next_seq += 1;
        off += static_cast<size_t>(n);
        left -= static_cast<size_t>(n);
    }

    ZcSendRec rec;
    rec.first_seq = first_seq;
    rec.last_seq = last_seq;
    rec.buf = buf;
    ctx->inflight_zc.push_back(std::move(rec));
    ctx->zc_busy.store(1, std::memory_order_release);
    g_zc_send.fetch_add(1, std::memory_order_relaxed);
    DrainFd(ctx.get());
    if (ctx->inflight_zc.empty())
    {
        ctx->zc_busy.store(0, std::memory_order_release);
    }
    else
    {
        Watch(conn);
    }
    return true;
}

void ZeroCopySend::Watch(const muduo::net::TcpConnectionPtr &conn)
{
    g_zc_conns.push_back(conn);
}

void ZeroCopySend::DrainLocal()
{
    auto &v = g_zc_conns;
    for (size_t i = 0; i < v.size();)
    {
        auto conn = v[i].lock();
        if (!conn)
        {
            v[i] = std::move(v.back());
            v.pop_back();
            continue;
        }
        auto ctx = GetConnContext(conn);
        if (!ctx)
        {
            v[i] = std::move(v.back());
            v.pop_back();
            continue;
        }

        DrainFd(ctx.get());

        if (ctx->inflight_zc.empty())
        {
            ctx->zc_busy.store(0, std::memory_order_release);
            KrpcConnectPool::GetInstance().MaybeMakeAvailable(conn);
            v[i] = std::move(v.back());
            v.pop_back();
            continue;
        }
        ++i;
    }
}

uint64_t ZeroCopySend::SendCount()
{
    return g_zc_send.load(std::memory_order_relaxed);
}

uint64_t ZeroCopySend::CopiedCount()
{
    return g_zc_copied.load(std::memory_order_relaxed);
}

uint64_t ZeroCopySend::CompleteCount()
{
    return g_zc_complete.load(std::memory_order_relaxed);
}
