#include "Krpcapplication.h"
#include "KrpcClientIo.h"
#include "KrpcConnectPool.h"
#include "Krpccontroller.h"
#include "KrpcLogger.h"
#include "../user.pb.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char **argv)
{
    KrpcApplication::Init(argc, argv);
    FLAGS_logbufsecs = 5;
    KrpcLogger logger("MyRPC");

    std::string ip = KrpcApplication::GetConfig().Load("rpcserverip");
    uint16_t port =
        static_cast<uint16_t>(std::atoi(KrpcApplication::GetConfig().Load("rpcserverport").c_str()));
    KrpcConnectPool::GetInstance().WarmUp(ip, port, 1);

    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false),
                                    google::protobuf::Service::STUB_OWNS_CHANNEL);

    Kuser::LoginRequest login_req;
    login_req.set_name("zhangsan");
    login_req.set_pwd("123456");
    Kuser::LoginResponse login_resp;
    Krpccontroller login_ctrl;
    stub.Login(&login_ctrl, &login_req, &login_resp, nullptr);
    if (login_ctrl.Failed() || login_resp.result().errcode() != 0)
    {
        std::cerr << "Login failed: " << login_ctrl.ErrorText() << std::endl;
        KrpcConnectPool::GetInstance().Shutdown();
        KrpcClientIo::Instance().Stop();
        return 1;
    }
    std::cout << "Login ok" << std::endl;

    Kuser::EchoBlobRequest blob_req;
    blob_req.set_body(std::string(32 * 1024, 'a'));
    Kuser::EchoBlobResponse blob_resp;
    Krpccontroller blob_ctrl;
    stub.EchoBlob(&blob_ctrl, &blob_req, &blob_resp, nullptr);
    if (blob_ctrl.Failed() || blob_resp.body().size() != blob_req.body().size())
    {
        std::cerr << "EchoBlob failed: " << blob_ctrl.ErrorText() << std::endl;
        KrpcConnectPool::GetInstance().Shutdown();
        KrpcClientIo::Instance().Stop();
        return 1;
    }
    std::cout << "EchoBlob ok echo_size=" << blob_resp.body().size() << std::endl;

    KrpcConnectPool::GetInstance().Shutdown();
    KrpcClientIo::Instance().Stop();
    return 0;
}
