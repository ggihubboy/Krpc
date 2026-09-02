#ifndef KRPC_RETRY_POLICY_H
#define KRPC_RETRY_POLICY_H

#include "RpcError.h"

// Automatic retries are restricted to failures that happen before any bytes
// are submitted to the connection. Replaying a sent request could duplicate a
// non-idempotent business operation.
inline bool IsSafePreSendRetry(int error_code)
{
    return error_code == kRpcConnectFail;
}

#endif
