#include "Krpcapplication.h"
#include "../user.pb.h"
#include "Krpccontroller.h"
#include "KrpcLogger.h"
#include "KrpcConnectPool.h"
#include "KrpcClientIo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

void send_request(int thread_id, std::atomic<int> &success_count, std::atomic<int> &fail_count, int requests_per_thread)
{
    (void)thread_id;
    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false), google::protobuf::Service::STUB_OWNS_CHANNEL);

    Kuser::LoginRequest request;
    request.set_name("zhangsan");
    request.set_pwd("123456");

    Kuser::LoginResponse response;
    Krpccontroller controller;

    for (int i = 0; i < requests_per_thread; ++i)
    {
        controller.Reset();
        stub.Login(&controller, &request, &response, nullptr);

        if (controller.Failed())
        {
            fail_count++;
        }
        else if (int{} == response.result().errcode())
        {
            success_count++;
        }
        else
        {
            fail_count++;
        }
    }
}

int main(int argc, char **argv)
{
    KrpcApplication::Init(argc, argv);
    FLAGS_logbufsecs = 5;
    KrpcLogger logger("MyRPC");

    std::string ip = KrpcApplication::GetConfig().Load("rpcserverip");
    uint16_t port = static_cast<uint16_t>(atoi(KrpcApplication::GetConfig().Load("rpcserverport").c_str()));

    const int thread_count = KrpcApplication::CpuCores();
    const int io_threads = KrpcApplication::ClientIoThreads();
    const int requests_per_thread = std::max(1000, 100000 / thread_count);

    KrpcConnectPool::GetInstance().WarmUp(ip, port, thread_count);

    LOG(INFO) << "cpu_cores=" << thread_count
              << " client_io_loops=" << io_threads
              << " client_threads=" << thread_count
              << " requests_per_thread=" << requests_per_thread;

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < thread_count; i++)
    {
        threads.emplace_back([i, &success_count, &fail_count, requests_per_thread]() {
            send_request(i, success_count, fail_count, requests_per_thread);
        });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    const int total = thread_count * requests_per_thread;

    LOG(INFO) << "Total requests: " << total;
    LOG(INFO) << "Success count: " << success_count;
    LOG(INFO) << "Fail count: " << fail_count;
    LOG(INFO) << "Elapsed time: " << elapsed.count() << " seconds";
    LOG(INFO) << "QPS: " << total / elapsed.count();

    if (KrpcApplication::EnableZeroCopy())
    {
        Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false), google::protobuf::Service::STUB_OWNS_CHANNEL);
        Kuser::EchoBlobRequest blob_req;
        blob_req.set_body(std::string(32 * 1024, 'a'));
        Kuser::EchoBlobResponse blob_resp;
        Krpccontroller blob_ctrl;
        stub.EchoBlob(&blob_ctrl, &blob_req, &blob_resp, nullptr);
        LOG(INFO) << "EchoBlob 32KB " << (blob_ctrl.Failed() ? blob_ctrl.ErrorText() : "ok")
                  << " echo_size=" << blob_resp.body().size();
    }

    KrpcConnectPool::GetInstance().Shutdown();
    KrpcClientIo::Instance().Stop();
    return 0;
}
