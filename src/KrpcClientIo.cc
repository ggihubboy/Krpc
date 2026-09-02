#include "KrpcClientIo.h"
#include "TimeoutWheel.h"
#include "ZeroCopySend.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

KrpcClientIo &KrpcClientIo::Instance()
{
    static KrpcClientIo io;
    return io;
}

void KrpcClientIo::EnsureStarted()
{
    static std::once_flag once;
    std::call_once(once, [this]() {
        unsigned cores = std::thread::hardware_concurrency();
        if (cores == 0)
        {
            cores = 1;
        }
        const int loop_count = std::min(8, std::max(4, static_cast<int>(cores)));
        threads_.reserve(static_cast<size_t>(loop_count));
        loops_.reserve(static_cast<size_t>(loop_count));
        auto init = [](muduo::net::EventLoop *loop) {
            loop->runEvery(0.01, []() {
                TimeoutWheel::Scan();
                ZeroCopySend::DrainLocal();
            });
        };
        for (int i = 0; i < loop_count; ++i)
        {
            auto thread = std::make_unique<muduo::net::EventLoopThread>(
                init, "KrpcClientIo-" + std::to_string(i));
            muduo::net::EventLoop *loop = thread->startLoop();
            loops_.push_back(loop);
            threads_.push_back(std::move(thread));
        }
        started_.store(true, std::memory_order_release);
    });
}

muduo::net::EventLoop *KrpcClientIo::NextLoop()
{
    EnsureStarted();
    const int n = static_cast<int>(loops_.size());
    const int idx = next_.fetch_add(1, std::memory_order_relaxed) % n;
    return loops_[static_cast<size_t>(idx)];
}

bool KrpcClientIo::Started() const
{
    return started_.load(std::memory_order_acquire);
}

void KrpcClientIo::Stop()
{
    if (!started_.load(std::memory_order_acquire))
    {
        return;
    }
    threads_.clear();
    loops_.clear();
    started_.store(false, std::memory_order_release);
}

KrpcClientIo::~KrpcClientIo()
{
    Stop();
}
