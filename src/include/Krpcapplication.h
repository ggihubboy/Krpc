#ifndef _Krpcapplication_H
#define _Krpcapplication_H

#include "Krpcchannel.h"
#include "Krpcconfig.h"
#include "Krpccontroller.h"

#include <cstdint>

class KrpcApplication
{
public:
    static void Init(int argc, char **argv);
    static KrpcApplication &GetInstance();
    static void deleteInstance();
    static Krpcconfig &GetConfig();
    static int RpcTimeoutMs();
    static int TcpKeepaliveIdleS();
    static int ConnIdleEvictMs();
    static int ZeroCopyThreshold();
    static bool EnableZeroCopy();
    static uint32_t RpcMaxBodyBytes();
    static int MaxInflightPerConn();
    static int ServerMaxPending();
    static int ServerShutdownGraceMs();
    static bool EnableAccessLog();
    static int CpuCores();
    static int ClientIoThreads();

private:
    KrpcApplication() = default;
    ~KrpcApplication() = default;
    KrpcApplication(const KrpcApplication &) = delete;
    KrpcApplication(KrpcApplication &&) = delete;

    static Krpcconfig m_config;
    static int m_rpc_timeout_ms;
    static int m_tcp_keepalive_idle_s;
    static int m_conn_idle_evict_ms;
    static int m_zerocopy_threshold;
    static int m_enable_zerocopy;
    static uint32_t m_rpc_max_body_bytes;
    static int m_max_inflight_per_conn;
    static int m_server_max_pending;
    static int m_server_shutdown_grace_ms;
    static int m_enable_access_log;
};

#endif
