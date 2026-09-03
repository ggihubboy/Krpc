#include "Krpcapplication.h"
#include "CircuitBreaker.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <muduo/base/Logging.h>
#include <thread>
#include <unistd.h>

Krpcconfig KrpcApplication::m_config;
int KrpcApplication::m_rpc_timeout_ms = 3000;
int KrpcApplication::m_tcp_keepalive_idle_s = 30;
int KrpcApplication::m_conn_idle_evict_ms = 60000;
int KrpcApplication::m_zerocopy_threshold = 16384;
int KrpcApplication::m_enable_zerocopy = 0;
uint32_t KrpcApplication::m_rpc_max_body_bytes = 16u * 1024u * 1024u;
int KrpcApplication::m_max_inflight_per_conn = 32;
int KrpcApplication::m_server_max_pending = 4096;
int KrpcApplication::m_server_shutdown_grace_ms = 5000;
int KrpcApplication::m_enable_access_log = 0;

static int ParseIntOr(const std::string &text, int fallback)
{
    if (text.empty())
    {
        return fallback;
    }
    try
    {
        return std::stoi(text);
    }
    catch (...)
    {
        return fallback;
    }
}

void KrpcApplication::Init(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cout << "格式: command -i <配置文件路径>" << std::endl;
        exit(EXIT_FAILURE);
    }

    int o = 0;
    std::string config_file;
    while (-1 != (o = getopt(argc, argv, "i:")))
    {
        switch (o)
        {
        case 'i':
            config_file = optarg;
            break;
        case '?':
        case ':':
            std::cout << "格式: command -i <配置文件路径>" << std::endl;
            exit(EXIT_FAILURE);
            break;
        default:
            break;
        }
    }

    m_config.LoadConfigFile(config_file.c_str());
    m_rpc_timeout_ms = std::max(1, ParseIntOr(m_config.Load("rpc_timeout_ms"), 3000));
    m_tcp_keepalive_idle_s = std::max(1, ParseIntOr(m_config.Load("tcp_keepalive_idle_s"), 30));
    m_conn_idle_evict_ms = std::max(0, ParseIntOr(m_config.Load("conn_idle_evict_ms"), 60000));
    m_zerocopy_threshold = std::max(0, ParseIntOr(m_config.Load("zerocopy_threshold"), 16384));
    m_enable_zerocopy = ParseIntOr(m_config.Load("enable_zerocopy"), 0);
    m_rpc_max_body_bytes = static_cast<uint32_t>(
        std::max(1024, ParseIntOr(m_config.Load("rpc_max_body_bytes"), 16 * 1024 * 1024)));
    m_max_inflight_per_conn = std::max(1, ParseIntOr(m_config.Load("max_inflight_per_conn"), 32));
    m_server_max_pending = std::max(1, ParseIntOr(m_config.Load("server_max_pending"), 4096));
    m_server_shutdown_grace_ms =
        std::max(0, ParseIntOr(m_config.Load("server_shutdown_grace_ms"), 5000));
    m_enable_access_log = ParseIntOr(m_config.Load("enable_access_log"), 0);
    CircuitBreaker::Instance().Configure(
        ParseIntOr(m_config.Load("circuit_fail_threshold"), 5),
        ParseIntOr(m_config.Load("circuit_reset_ms"), 1000));
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
}

KrpcApplication &KrpcApplication::GetInstance()
{
    static KrpcApplication application;
    return application;
}

void KrpcApplication::deleteInstance()
{
}

Krpcconfig &KrpcApplication::GetConfig()
{
    return m_config;
}

int KrpcApplication::RpcTimeoutMs()
{
    return m_rpc_timeout_ms;
}

int KrpcApplication::TcpKeepaliveIdleS()
{
    return m_tcp_keepalive_idle_s;
}

int KrpcApplication::ConnIdleEvictMs()
{
    return m_conn_idle_evict_ms;
}

int KrpcApplication::ZeroCopyThreshold()
{
    return m_zerocopy_threshold;
}

bool KrpcApplication::EnableZeroCopy()
{
    return m_enable_zerocopy != 0;
}

uint32_t KrpcApplication::RpcMaxBodyBytes()
{
    return m_rpc_max_body_bytes;
}

int KrpcApplication::MaxInflightPerConn()
{
    return m_max_inflight_per_conn;
}

int KrpcApplication::ServerMaxPending()
{
    return m_server_max_pending;
}

int KrpcApplication::ServerShutdownGraceMs()
{
    return m_server_shutdown_grace_ms;
}

bool KrpcApplication::EnableAccessLog()
{
    return m_enable_access_log != 0;
}

int KrpcApplication::CpuCores()
{
    const unsigned cores = std::thread::hardware_concurrency();
    return cores == 0 ? 1 : static_cast<int>(cores);
}

int KrpcApplication::ClientIoThreads()
{
    return std::min(8, std::max(4, CpuCores()));
}
