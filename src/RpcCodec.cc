#include "RpcCodec.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>

bool RpcFrameFits(uint32_t total_len, uint32_t header_len, uint32_t max_bytes, uint32_t *payload_len)
{
    if (total_len < 4 || total_len > max_bytes)
    {
        return false;
    }
    if (header_len > total_len - 4)
    {
        return false;
    }
    if (payload_len != nullptr)
    {
        *payload_len = total_len - 4 - header_len;
    }
    return true;
}

RpcDecodeStatus DecodeRpcFrame(muduo::net::Buffer *buffer,
                               uint32_t max_bytes,
                               std::string *header,
                               std::string *payload)
{
    if (buffer == nullptr || header == nullptr || payload == nullptr)
    {
        return RpcDecodeStatus::Corrupt;
    }
    if (buffer->readableBytes() < 4)
    {
        return RpcDecodeStatus::NeedMore;
    }

    uint32_t total_len_n = 0;
    std::memcpy(&total_len_n, buffer->peek(), 4);
    const uint32_t total_len = ntohl(total_len_n);
    if (total_len < 4 || total_len > max_bytes)
    {
        return RpcDecodeStatus::Corrupt;
    }

    const uint64_t need = 4ull + static_cast<uint64_t>(total_len);
    if (buffer->readableBytes() < need)
    {
        return RpcDecodeStatus::NeedMore;
    }

    buffer->retrieve(4);

    uint32_t header_len_n = 0;
    std::memcpy(&header_len_n, buffer->peek(), 4);
    const uint32_t header_len = ntohl(header_len_n);
    uint32_t payload_len = 0;
    if (!RpcFrameFits(total_len, header_len, max_bytes, &payload_len))
    {
        return RpcDecodeStatus::Corrupt;
    }
    buffer->retrieve(4);

    header->assign(buffer->peek(), header_len);
    buffer->retrieve(header_len);
    payload->assign(buffer->peek(), payload_len);
    buffer->retrieve(payload_len);
    return RpcDecodeStatus::Ok;
}

bool EncodeRpcFrame(const std::string &header,
                    const std::string &payload,
                    uint32_t max_bytes,
                    std::string *out)
{
    if (out == nullptr)
    {
        return false;
    }
    const uint64_t header_len = header.size();
    const uint64_t payload_len = payload.size();
    const uint64_t total_len = 4ull + header_len + payload_len;
    if (header_len > UINT32_MAX || payload_len > UINT32_MAX || total_len > max_bytes)
    {
        return false;
    }

    const uint32_t net_total = htonl(static_cast<uint32_t>(total_len));
    const uint32_t net_header = htonl(static_cast<uint32_t>(header_len));
    out->clear();
    out->reserve(4 + static_cast<size_t>(total_len));
    out->append(reinterpret_cast<const char *>(&net_total), 4);
    out->append(reinterpret_cast<const char *>(&net_header), 4);
    out->append(header);
    out->append(payload);
    return true;
}
