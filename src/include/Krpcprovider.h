#ifndef _Krpcprovider_H__
#define _Krpcprovider_H__

#include "google/protobuf/service.h"
#include "ShutdownState.h"
#include "zookeeperutil.h"

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/base/ThreadPool.h>
#include <google/protobuf/descriptor.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class Krpccontroller;

class KrpcProvider
{
public:
    KrpcProvider();
    void NotifyService(google::protobuf::Service *service);
    ~KrpcProvider();
    void Run();
    void RequestStop();

private:
    muduo::net::EventLoop event_loop;
    muduo::ThreadPool m_thread_pool;
    ZkClient m_zk;
    std::shared_ptr<muduo::net::TcpServer> m_server;
    ShutdownState m_shutdown;
    bool m_drain_started = false;
    std::atomic<int> m_pending_jobs{0};

    struct ServiceInfo
    {
        google::protobuf::Service *service;
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor *> method_map;
    };
    std::unordered_map<std::string, ServiceInfo> service_map;

    void OnConnection(const muduo::net::TcpConnectionPtr &conn);
    void OnMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer, muduo::Timestamp receive_time);
    void SendFrame(const muduo::net::TcpConnectionPtr &conn, std::string frame);
    void SendError(const muduo::net::TcpConnectionPtr &conn, uint64_t request_id, int code, const std::string &msg);
    void SendRpcResponse(const muduo::net::TcpConnectionPtr &conn,
                         google::protobuf::Message *response,
                         google::protobuf::Message *request,
                         uint64_t request_id,
                         Krpccontroller *controller);
};

#endif
