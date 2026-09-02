#ifndef KRPC_RPC_CODEC_H
#define KRPC_RPC_CODEC_H

#include <muduo/net/Buffer.h>

#include <cstdint>
#include <string>

// 请求和响应统一用同一套长度前缀帧：
//   total_len(4, 网络序) + header_len(4, 网络序) + header + payload
// total_len = 4 + header_len + payload_len（不含最前面那个 total_len 自己）。
// 任何长度越界、溢出、超 max_bytes 都视为 Corrupt，调用方应关掉连接。
enum class RpcDecodeStatus
{
    NeedMore,
    Ok,
    Corrupt
};

bool RpcFrameFits(uint32_t total_len, uint32_t header_len, uint32_t max_bytes, uint32_t *payload_len);

RpcDecodeStatus DecodeRpcFrame(muduo::net::Buffer *buffer,
                               uint32_t max_bytes,
                               std::string *header,
                               std::string *payload);

bool EncodeRpcFrame(const std::string &header,
                    const std::string &payload,
                    uint32_t max_bytes,
                    std::string *out);

#endif
