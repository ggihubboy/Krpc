#include "Krpcapplication.h"
#include "KrpcClientIo.h"
#include "KrpcConnectPool.h"
#include "Krpccontroller.h"
#include "KrpcLogger.h"
#include "RpcMetrics.h"
#include "user.pb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
int EnvInt(const char *name, int fallback)
{
    const char *text = std::getenv(name);
    if (text == nullptr || *text == '\0')
    {
        return fallback;
    }
    try
    {
        return std::stoi(text);
    }
    catch (...)
    {
        return fallback;
    }
}

void OneCall(int payload_bytes, std::atomic<int> *success, std::atomic<int> *fail)
{
    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false),
                                    google::protobuf::Service::STUB_OWNS_CHANNEL);
    Krpccontroller controller;
    if (payload_bytes <= 0)
    {
        Kuser::LoginRequest request;
        Kuser::LoginResponse response;
        request.set_name("zhangsan");
        request.set_pwd("123456");
        stub.Login(&controller, &request, &response, nullptr);
        if (!controller.Failed() && response.result().errcode() == 0)
        {
            success->fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    else
    {
        Kuser::EchoBlobRequest request;
        Kuser::EchoBlobResponse response;
        request.set_body(std::string(static_cast<size_t>(payload_bytes), 'a'));
        stub.EchoBlob(&controller, &request, &response, nullptr);
        if (!controller.Failed() &&
            static_cast<int>(response.body().size()) == payload_bytes)
        {
            success->fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    fail->fetch_add(1, std::memory_order_relaxed);
}
} // namespace

int main(int argc, char **argv)
{
    KrpcApplication::Init(argc, argv);
    FLAGS_logbufsecs = 5;
    KrpcLogger logger("krpc_bench");

    const int thread_count = std::max(1, EnvInt("KRPC_BENCH_THREADS", KrpcApplication::CpuCores()));
    const int requests_per_thread =
        std::max(1, EnvInt("KRPC_BENCH_REQUESTS", std::max(1000, 100000 / thread_count)));
    const int payload_bytes = std::max(0, EnvInt("KRPC_BENCH_PAYLOAD", 0));

    std::string ip = KrpcApplication::GetConfig().Load("rpcserverip");
    uint16_t port =
        static_cast<uint16_t>(std::atoi(KrpcApplication::GetConfig().Load("rpcserverport").c_str()));
    KrpcConnectPool::GetInstance().WarmUp(ip, port, thread_count);

    std::atomic<int> success{0};
    std::atomic<int> fail{0};
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i)
    {
        workers.emplace_back([&]() {
            for (int n = 0; n < requests_per_thread; ++n)
            {
                OneCall(payload_bytes, &success, &fail);
            }
        });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const int total = thread_count * requests_per_thread;
    const double seconds = std::max(elapsed.count(), 1e-9);
    const RpcMetricsSnapshot snap = RpcMetrics::Instance().Snapshot();

    std::cout << "krpc_bench"
              << " threads=" << thread_count
              << " requests=" << total
              << " payload=" << payload_bytes
              << " success=" << success.load()
              << " fail=" << fail.load()
              << " seconds=" << seconds
              << " qps=" << static_cast<int>(total / seconds)
              << " error_rate=" << (static_cast<double>(fail.load()) / total)
              << " " << snap.ToString()
              << std::endl;

    KrpcConnectPool::GetInstance().Shutdown();
    KrpcClientIo::Instance().Stop();
    return fail.load() == 0 ? 0 : 1;
}
