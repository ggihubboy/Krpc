#ifndef KRPC_CONNECT_POOL_H
#define KRPC_CONNECT_POOL_H

#include "LockFreeQueue.h"
#include "RpcError.h"

#include <muduo/net/Buffer.h>
#include <muduo/net/TcpClient.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/Callbacks.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ConnectionBucket
{
    std::string ip;
    uint16_t port = 0;
    std::unique_ptr<MPMCQueue<muduo::net::TcpConnectionPtr>> available;
    std::mutex wait_mtx;
    std::condition_variable wait_cv;
    std::mutex clients_mtx;
    std::vector<std::unique_ptr<muduo::net::TcpClient>> clients;
    std::atomic<int> ready{0};
    std::atomic<int> connecting{0};

    explicit ConnectionBucket(size_t capacity)
        : available(std::make_unique<MPMCQueue<muduo::net::TcpConnectionPtr>>(capacity))
    {
    }
};

class KrpcConnectPool
{
public:
    static KrpcConnectPool &GetInstance();

    muduo::net::TcpConnectionPtr BorrowConnection(const std::string &ip, uint16_t port, int timeout_ms);
    void MaybeMakeAvailable(const muduo::net::TcpConnectionPtr &conn);
    void CloseConnection(const muduo::net::TcpConnectionPtr &conn);
    void WarmUp(const std::string &ip, uint16_t port, int count);
    void Shutdown();

private:
    KrpcConnectPool() = default;
    ~KrpcConnectPool();

    ConnectionBucket *GetBucket(const std::string &ip, uint16_t port);
    void StartConnect(ConnectionBucket *bucket);
    void PushAvailable(ConnectionBucket *bucket, const muduo::net::TcpConnectionPtr &conn);

    static void OnConnection(ConnectionBucket *bucket, const muduo::net::TcpConnectionPtr &conn);
    static void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                          muduo::net::Buffer *buffer,
                          muduo::Timestamp receive_time);
    static void FailAllPending(const muduo::net::TcpConnectionPtr &conn,
                               const std::string &err,
                               int error_code = kRpcConnectFail);

    static constexpr int kMaxConnPerNode = 1024;
    static constexpr int kMaxConnecting = 16;

    std::mutex m_global_mtx;
    std::unordered_map<std::string, std::unique_ptr<ConnectionBucket>> m_pools;
};

#endif
