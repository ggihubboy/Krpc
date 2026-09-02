#include "TcpSockUtil.h"
#include "KrpcLogger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

static std::string SockToIpPort(const sockaddr_storage &ss)
{
    char ip[INET6_ADDRSTRLEN] = {0};
    uint16_t port = 0;
    if (ss.ss_family == AF_INET)
    {
        const auto *addr = reinterpret_cast<const sockaddr_in *>(&ss);
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
        port = ntohs(addr->sin_port);
    }
    else if (ss.ss_family == AF_INET6)
    {
        const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&ss);
        if (IN6_IS_ADDR_V4MAPPED(&addr->sin6_addr))
        {
            inet_ntop(AF_INET, &addr->sin6_addr.s6_addr[12], ip, sizeof(ip));
        }
        else
        {
            inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));
        }
        port = ntohs(addr->sin6_port);
    }
    return std::string(ip) + ":" + std::to_string(port);
}

int LookupTcpFd(const muduo::net::TcpConnectionPtr &conn)
{
    if (!conn)
    {
        return -1;
    }

    const std::string want_local = conn->localAddress().toIpPort();
    const std::string want_peer = conn->peerAddress().toIpPort();

    DIR *dir = opendir("/proc/self/fd");
    if (dir == nullptr)
    {
        return -1;
    }

    int found = -1;
    while (dirent *ent = readdir(dir))
    {
        if (ent->d_name[0] == '.')
        {
            continue;
        }
        const int fd = atoi(ent->d_name);
        if (fd < 0)
        {
            continue;
        }

        sockaddr_storage local{};
        sockaddr_storage peer{};
        socklen_t local_len = sizeof(local);
        socklen_t peer_len = sizeof(peer);
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&local), &local_len) != 0)
        {
            continue;
        }
        if (getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peer_len) != 0)
        {
            continue;
        }
        if (SockToIpPort(local) == want_local && SockToIpPort(peer) == want_peer)
        {
            found = fd;
            break;
        }
    }
    closedir(dir);
    return found;
}

void SetTcpKeepalive(int fd, int idle_s)
{
    if (fd < 0 || idle_s <= 0)
    {
        return;
    }
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) != 0)
    {
        LOG(ERROR) << "SO_KEEPALIVE failed, fd=" << fd;
        return;
    }
    int idle = idle_s;
    int intvl = 10;
    int cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

bool EnableSockZeroCopy(int fd)
{
    if (fd < 0)
    {
        return false;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &on, sizeof(on)) != 0)
    {
        LOG(INFO) << "SO_ZEROCOPY not available, fd=" << fd << " errno=" << errno;
        return false;
    }
    return true;
}
