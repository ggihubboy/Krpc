#ifndef _Krpcchannel_h_
#define _Krpcchannel_h_

#include <google/protobuf/service.h>

#include <cstdint>
#include <string>

class KrpcChannel : public google::protobuf::RpcChannel
{
public:
    explicit KrpcChannel(bool connectNow = false) { (void)connectNow; }
    ~KrpcChannel() override = default;

    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override;

private:
    bool IssueOnce(const std::string &node,
                   const std::string &payload,
                   uint64_t request_id,
                   google::protobuf::RpcController *controller,
                   google::protobuf::Message *response,
                   google::protobuf::Closure *done,
                   int timeout_ms,
                   bool notify_done_on_immediate_failure,
                   bool *pre_send_failure,
                   const std::string &service,
                   const std::string &method,
                   int64_t start_us);
};

#endif
