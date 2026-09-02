#ifndef KRPC_RPC_OBJECT_POOL_H
#define KRPC_RPC_OBJECT_POOL_H

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <functional>
#include <vector>

// 可重置的 RPC 完成回调：Run 里不再自杀，用完还回 TLS 池。
class KrpcClosure : public google::protobuf::Closure
{
public:
    void Reset(std::function<void()> cb);
    void Run() override;

private:
    std::function<void()> cb_;
};

// 按消息类型分池，存在业务线程 thread_local 里。
// 借和还必须在同一条业务线程（SendRpcResponse / done->Run 所在线程）。
class RpcObjectPool
{
public:
    static google::protobuf::Message *AcquireRequest(
        google::protobuf::Service *service,
        const google::protobuf::MethodDescriptor *method);
    static google::protobuf::Message *AcquireResponse(
        google::protobuf::Service *service,
        const google::protobuf::MethodDescriptor *method);
    static KrpcClosure *AcquireClosure();
    static void Release(google::protobuf::Message *msg);
    static void RecycleClosure(KrpcClosure *c);
};

#endif
