#ifndef _Krpccontroller_H
#define _Krpccontroller_H

#include "RpcError.h"

#include <google/protobuf/service.h>
#include <string>
//用于描述RPC调用的控制器
//其主要作用是跟踪RPC方法调用的状态、错误信息并提供控制功能(如取消调用)。
class Krpccontroller : public google::protobuf::RpcController
{
public:
    Krpccontroller();
    void Reset() override;
    bool Failed() const override;
    std::string ErrorText() const override;
    void SetFailed(const std::string &reason) override;

    void SetFailed(int error_code, const std::string &reason);
    int ErrorCode() const;

//目前未实现具体的功能
    void StartCancel() override;
    bool IsCanceled() const override;
    void NotifyOnCancel(google::protobuf::Closure *callback) override;
private:
    bool m_failed;//RPC方法执行过程中的状态
    int m_error_code;
    std::string m_errText;//RPC方法执行过程中的错误信息
};

// 保留 protobuf RpcController 接口兼容性；使用 Krpccontroller 时额外保存稳定错误码。
void SetRpcFailed(google::protobuf::RpcController *controller,
                  int error_code,
                  const std::string &reason);

#endif