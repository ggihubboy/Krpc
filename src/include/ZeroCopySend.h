#ifndef KRPC_ZERO_COPY_SEND_H
#define KRPC_ZERO_COPY_SEND_H

#include <muduo/net/TcpConnection.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class ConnContext;

// 仅 payload ≥ 阈值、outputBuffer 为空、套接字已 SO_ZEROCOPY 时走 MSG_ZEROCOPY。
// 小包仍用 Muduo send，避免 pin 页成本大于一次拷贝。
class ZeroCopySend
{
public:
    static bool ShouldUse(size_t nbytes, const muduo::net::TcpConnectionPtr &conn);

    // true 表示已经发出（含部分发出后的收尾）；false 表示调用方应走 conn->send。
    static bool TrySend(const muduo::net::TcpConnectionPtr &conn,
                        const std::shared_ptr<std::vector<char>> &buf);

    static void Watch(const muduo::net::TcpConnectionPtr &conn);
    static void DrainLocal();

    static uint64_t SendCount();
    static uint64_t CopiedCount();
    static uint64_t CompleteCount();
};

#endif
