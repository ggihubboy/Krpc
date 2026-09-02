#include "CircuitBreaker.h"
#include "ConsistentHash.h"
#include "LockFreeQueue.h"
#include "RpcCodec.h"
#include "RpcPendingCall.h"

#include <muduo/net/Buffer.h>

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_failed = 0;

void Expect(bool cond, const char *msg)
{
    if (!cond)
    {
        std::cerr << "FAIL: " << msg << std::endl;
        ++g_failed;
    }
}
} // namespace

static void TestRpcFrameFits()
{
    uint32_t payload = 0;
    Expect(RpcFrameFits(8, 4, 16, &payload) && payload == 0, "min valid frame");
    Expect(!RpcFrameFits(3, 0, 16, &payload), "total_len < 4");
    Expect(!RpcFrameFits(100, 0, 16, &payload), "exceeds max_bytes");
    Expect(!RpcFrameFits(8, 9, 16, &payload), "header_len overflow");
    Expect(RpcFrameFits(12, 4, 16, &payload) && payload == 4, "payload 4");
    // uint32 wrap style: huge total_len must fail max check, never pass as small need
    Expect(!RpcFrameFits(0xFFFFFFFFu, 4, 16 * 1024 * 1024, &payload), "huge total_len rejected");
}

static void TestEncodeDecode()
{
    std::string frame;
    Expect(EncodeRpcFrame("hdr", "body", 1024, &frame), "encode ok");
    muduo::net::Buffer buf;
    buf.append(frame.data(), frame.size());
    std::string header;
    std::string payload;
    Expect(DecodeRpcFrame(&buf, 1024, &header, &payload) == RpcDecodeStatus::Ok, "decode ok");
    Expect(header == "hdr" && payload == "body", "roundtrip");
    Expect(buf.readableBytes() == 0, "buffer consumed");

    muduo::net::Buffer partial;
    partial.append(frame.data(), 2);
    Expect(DecodeRpcFrame(&partial, 1024, &header, &payload) == RpcDecodeStatus::NeedMore, "need more");

    muduo::net::Buffer bad;
    uint32_t huge = htonl(0xFFFFFFFFu);
    bad.append(&huge, 4);
    Expect(DecodeRpcFrame(&bad, 1024, &header, &payload) == RpcDecodeStatus::Corrupt, "overflow corrupt");

    std::string too_big;
    Expect(!EncodeRpcFrame(std::string(100, 'h'), "p", 16, &too_big), "encode reject oversized");
}

static void TestCircuitHalfOpen()
{
    CircuitBreaker &cb = CircuitBreaker::Instance();
    cb.Configure(2, 20);
    const std::string node = "half-open-node:1";
    Expect(cb.AllowRequest(node), "closed allow");
    cb.RecordFailure(node);
    Expect(cb.AllowRequest(node), "one fail still closed");
    cb.RecordFailure(node);
    Expect(!cb.AllowRequest(node), "open reject");

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::atomic<int> allowed{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
    {
        ts.emplace_back([&]() {
            if (cb.AllowRequest(node))
            {
                allowed.fetch_add(1);
            }
        });
    }
    for (auto &t : ts)
    {
        t.join();
    }
    Expect(allowed.load() == 1, "half-open only one probe");
    cb.RecordSuccess(node);
    Expect(cb.AllowRequest(node), "closed after success");
}

static void TestPendingCompleteOnce()
{
    auto call = std::make_shared<RpcPendingCall>();
    Expect(call->TryComplete(true, "", false), "first complete");
    Expect(!call->TryComplete(false, "late", false), "second complete rejected");
    Expect(call->ok, "winner result kept");
}

static void TestHashWriteSerialized()
{
    ConsistentHash ring;
    ring.UpdateNodes({"10.0.0.1:8000", "10.0.0.2:8000"});
    std::string a = ring.GetTargetNode("k1");
    Expect(!a.empty(), "pick node");
    ring.UpdateNodes({"10.0.0.3:8000"});
    std::string b = ring.GetTargetNode("k1");
    Expect(b == "10.0.0.3:8000", "update replaces nodes");
}

static void TestMpmc()
{
    MPMCQueue<int> q(8);
    Expect(q.push(1) && q.push(2), "push");
    int v = 0;
    Expect(q.pop(v) && v == 1, "pop 1");
    Expect(q.pop(v) && v == 2, "pop 2");
    Expect(!q.pop(v), "empty");
}

int main()
{
    TestRpcFrameFits();
    TestEncodeDecode();
    TestCircuitHalfOpen();
    TestPendingCompleteOnce();
    TestHashWriteSerialized();
    TestMpmc();
    if (g_failed != 0)
    {
        std::cerr << g_failed << " assertion(s) failed" << std::endl;
        return 1;
    }
    std::cout << "krpc_tests ok" << std::endl;
    return 0;
}
