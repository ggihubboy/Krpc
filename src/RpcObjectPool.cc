#include "RpcObjectPool.h"

#include <unordered_map>

namespace
{
constexpr size_t kMaxPerType = 64;

thread_local std::unordered_map<const google::protobuf::Descriptor *,
                                std::vector<google::protobuf::Message *>>
    g_msg_pool;
thread_local std::vector<KrpcClosure *> g_closure_pool;

google::protobuf::Message *AcquireByPrototype(const google::protobuf::Message &prototype)
{
    const google::protobuf::Descriptor *desc = prototype.GetDescriptor();
    auto &pool = g_msg_pool[desc];
    if (!pool.empty())
    {
        google::protobuf::Message *msg = pool.back();
        pool.pop_back();
        return msg;
    }
    return prototype.New();
}
} // namespace

void KrpcClosure::Reset(std::function<void()> cb)
{
    cb_ = std::move(cb);
}

void KrpcClosure::Run()
{
    auto cb = std::move(cb_);
    if (cb)
    {
        cb();
    }
    RpcObjectPool::RecycleClosure(this);
}

google::protobuf::Message *RpcObjectPool::AcquireRequest(
    google::protobuf::Service *service,
    const google::protobuf::MethodDescriptor *method)
{
    return AcquireByPrototype(service->GetRequestPrototype(method));
}

google::protobuf::Message *RpcObjectPool::AcquireResponse(
    google::protobuf::Service *service,
    const google::protobuf::MethodDescriptor *method)
{
    return AcquireByPrototype(service->GetResponsePrototype(method));
}

KrpcClosure *RpcObjectPool::AcquireClosure()
{
    if (!g_closure_pool.empty())
    {
        KrpcClosure *c = g_closure_pool.back();
        g_closure_pool.pop_back();
        return c;
    }
    return new KrpcClosure();
}

void RpcObjectPool::Release(google::protobuf::Message *msg)
{
    if (msg == nullptr)
    {
        return;
    }
    const google::protobuf::Descriptor *desc = msg->GetDescriptor();
    auto &pool = g_msg_pool[desc];
    if (pool.size() >= kMaxPerType)
    {
        delete msg;
        return;
    }
    msg->Clear();
    pool.push_back(msg);
}

void RpcObjectPool::RecycleClosure(KrpcClosure *c)
{
    if (c == nullptr)
    {
        return;
    }
    c->Reset(nullptr);
    if (g_closure_pool.size() >= kMaxPerType)
    {
        delete c;
        return;
    }
    g_closure_pool.push_back(c);
}
