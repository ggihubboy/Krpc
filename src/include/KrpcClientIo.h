#ifndef KRPC_CLIENT_IO_H
#define KRPC_CLIENT_IO_H

#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>

#include <atomic>
#include <memory>
#include <vector>

// 客户端 IO 线程池：连接按 round-robin 绑到不同 EventLoop。
class KrpcClientIo
{
public:
    static KrpcClientIo &Instance();

    // 取下一条 loop，用于新建连接
    muduo::net::EventLoop *NextLoop();
    void Stop();
    bool Started() const;

private:
    KrpcClientIo() = default;
    ~KrpcClientIo();
    KrpcClientIo(const KrpcClientIo &) = delete;
    KrpcClientIo &operator=(const KrpcClientIo &) = delete;

    void EnsureStarted();

    std::vector<std::unique_ptr<muduo::net::EventLoopThread>> threads_;
    std::vector<muduo::net::EventLoop *> loops_;
    std::atomic<int> next_{0};
    std::atomic<bool> started_{false};
};

#endif
