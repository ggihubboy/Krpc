#ifndef KRPC_TCP_SOCK_UTIL_H
#define KRPC_TCP_SOCK_UTIL_H

#include <muduo/net/TcpConnection.h>

// Muduo 的 TcpConnection 不公开 fd，建连时按本端/对端地址反查一次并缓存。
int LookupTcpFd(const muduo::net::TcpConnectionPtr &conn);

// idle_s 对应 TCP_KEEPIDLE；探测间隔 10s、最多 3 次，与计划一致。
void SetTcpKeepalive(int fd, int idle_s);

// 失败则返回 false，调用方永久降级为普通 send。
bool EnableSockZeroCopy(int fd);

#endif
