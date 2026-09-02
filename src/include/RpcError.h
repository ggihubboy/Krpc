#ifndef KRPC_RPC_ERROR_H
#define KRPC_RPC_ERROR_H

// 业务/框架错误码，放在 RpcMeta.error_code。0 表示成功。
enum RpcErrorCode
{
    kRpcOk = 0,
    kRpcTimeout = 1,
    kRpcOverloaded = 2,
    kRpcNoService = 3,
    kRpcNoMethod = 4,
    kRpcBadRequest = 5,
    kRpcInternal = 6,
    kRpcCircuitOpen = 7,
    kRpcConnectFail = 8
};

#endif
