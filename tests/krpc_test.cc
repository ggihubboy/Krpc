#include "CircuitBreaker.h"
#include "ConsistentHash.h"
#include "Krpccontroller.h"
#include "LockFreeQueue.h"
#include "RpcCodec.h"
#include "RpcError.h"
#include "RpcPendingCall.h"
#include "RpcMetrics.h"
#include "RetryPolicy.h"
#include "ShutdownState.h"
#include "ZkHandleGuard.h"
#include "PendingWork.h"
#include "RetryAttemptState.h"

#include <muduo/net/Buffer.h>

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <future>
#include <stdexcept>
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

    Expect(DecodeRpcFrame(nullptr, 1024, &header, &payload) == RpcDecodeStatus::Corrupt,
           "null buffer rejected");
    Expect(DecodeRpcFrame(&buf, 1024, nullptr, &payload) == RpcDecodeStatus::Corrupt,
           "null output rejected");

    muduo::net::Buffer incomplete;
    incomplete.append(frame.data(), frame.size() - 1);
    const size_t before = incomplete.readableBytes();
    Expect(DecodeRpcFrame(&incomplete, 1024, &header, &payload) == RpcDecodeStatus::NeedMore,
           "incomplete frame waits");
    Expect(incomplete.readableBytes() == before, "incomplete frame is not consumed");

    muduo::net::Buffer pipelined;
    pipelined.append(frame);
    pipelined.append(frame);
    Expect(DecodeRpcFrame(&pipelined, 1024, &header, &payload) == RpcDecodeStatus::Ok,
           "first pipelined frame");
    Expect(pipelined.readableBytes() == frame.size(), "second pipelined frame retained");
    Expect(DecodeRpcFrame(&pipelined, 1024, &header, &payload) == RpcDecodeStatus::Ok,
           "second pipelined frame");
    Expect(pipelined.readableBytes() == 0, "pipeline consumed exactly");
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

    auto delayed = std::make_shared<RpcPendingCall>();
    std::thread publisher([delayed]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        delayed->TryComplete(false, "expected failure", true, kRpcTimeout);
    });
    Expect(delayed->Wait(100), "wait wakes after publication");
    publisher.join();
    Expect(!delayed->ok && delayed->err == "expected failure", "published error visible");
    Expect(delayed->error_code == kRpcTimeout, "published error code visible");
    Expect(delayed->close_on_finish, "published close flag visible");

    auto raced = std::make_shared<RpcPendingCall>();
    std::atomic<int> winners{0};
    std::vector<std::thread> racers;
    for (int i = 0; i < 16; ++i)
    {
        racers.emplace_back([raced, &winners, i]() {
            if (raced->TryComplete(i == 0, std::to_string(i), false))
            {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &racer : racers)
    {
        racer.join();
    }
    Expect(winners.load(std::memory_order_relaxed) == 1, "concurrent completion has one winner");
}

static void TestStructuredControllerError()
{
    Krpccontroller controller;
    Expect(controller.ErrorCode() == kRpcOk, "new controller has ok code");
    controller.SetFailed(kRpcTimeout, "rpc timeout");
    Expect(controller.Failed(), "structured failure marks controller failed");
    Expect(controller.ErrorCode() == kRpcTimeout, "structured failure preserves error code");
    Expect(controller.ErrorText() == "rpc timeout", "structured failure preserves message");
    controller.Reset();
    Expect(!controller.Failed(), "reset clears failure");
    Expect(controller.ErrorCode() == kRpcOk, "reset clears error code");

    google::protobuf::RpcController *base = &controller;
    SetRpcFailed(base, kRpcConnectFail, "connect failed");
    Expect(controller.ErrorCode() == kRpcConnectFail, "generic controller helper preserves code");
}

static void TestShutdownDrainState()
{
    ShutdownState state(100);
    Expect(!state.Requested(), "shutdown initially not requested");
    Expect(!state.ShouldStop(0, 1000), "running server must not stop");

    state.Request();
    Expect(state.Requested(), "shutdown request is visible");
    Expect(!state.ShouldStop(2, 1000), "inflight work drains before deadline");
    Expect(!state.ShouldStop(1, 1099), "drain continues up to deadline");
    Expect(state.ShouldStop(1, 1100), "drain stops at deadline");

    ShutdownState idle(100);
    idle.Request();
    Expect(idle.ShouldStop(0, 2000), "idle server stops immediately");
}

static void TestSafeRetryPolicy()
{
    Expect(IsSafePreSendRetry(kRpcConnectFail), "connect failure before send is retryable");
    Expect(!IsSafePreSendRetry(kRpcTimeout), "timeout may have reached server");
    Expect(!IsSafePreSendRetry(kRpcOverloaded), "server rejection is not replayed implicitly");
    Expect(!IsSafePreSendRetry(kRpcBadRequest), "bad request is not retryable");
}

static void TestRpcMetrics()
{
    RpcMetrics metrics;
    metrics.RecordStarted();
    metrics.RecordFinished(kRpcOk, 800);
    metrics.RecordStarted();
    metrics.RecordFinished(kRpcOk, 900);
    metrics.RecordStarted();
    metrics.RecordFinished(kRpcTimeout, 12'000);
    metrics.RecordStarted();
    metrics.RecordFinished(kRpcOverloaded, 60'000);

    const RpcMetricsSnapshot snapshot = metrics.Snapshot();
    Expect(snapshot.total == 4, "metrics count total calls");
    Expect(snapshot.success == 2, "metrics count success");
    Expect(snapshot.timeout == 1, "metrics count timeout");
    Expect(snapshot.overloaded == 1, "metrics count overload");
    Expect(snapshot.inflight == 0, "metrics inflight returns to zero");
    Expect(snapshot.ApproxPercentileUs(0.50) == 1000, "metrics p50 fixed bucket");
    Expect(snapshot.ApproxPercentileUs(0.99) == 100000, "metrics p99 fixed bucket");
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

    ring.UpdateNodes({"10.0.0.1:8000", "10.0.0.2:8000"});
    const std::string primary = ring.GetTargetNode("same-key");
    const std::string alternate = ring.GetTargetNode("same-key", primary);
    Expect(!primary.empty() && !alternate.empty(), "primary and alternate nodes available");
    Expect(primary != alternate, "excluded node is not selected");
}

static void TestMpmc()
{
    MPMCQueue<int> q(8);
    Expect(q.push(1) && q.push(2), "push");
    int v = 0;
    Expect(q.pop(v) && v == 1, "pop 1");
    Expect(q.pop(v) && v == 2, "pop 2");
    Expect(!q.pop(v), "empty");

    MPMCQueue<int> full(2);
    Expect(full.push(1) && full.push(2), "fill bounded queue");
    Expect(!full.push(3), "full bounded queue rejects push");

    bool invalid_capacity_rejected = false;
    try
    {
        MPMCQueue<int> invalid(3);
        (void)invalid;
    }
    catch (const std::invalid_argument &)
    {
        invalid_capacity_rejected = true;
    }
    Expect(invalid_capacity_rejected, "non-power-of-two capacity rejected");
}

static void TestZkHandleGuardSerializesReplacement()
{
    std::atomic<int> closes{0};
    ZkHandleGuard guard([&closes](zhandle_t *) { closes.fetch_add(1, std::memory_order_relaxed); });
    auto *first = reinterpret_cast<zhandle_t *>(0x1);
    auto *second = reinterpret_cast<zhandle_t *>(0x2);
    guard.Replace(first);

    std::promise<void> entered;
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::thread reader([&]() {
        const auto called = guard.WithHandle([&](zhandle_t *zh) {
            Expect(zh == first, "guard exposes current handle");
            entered.set_value();
            release_future.wait();
            return true;
        });
        Expect(called.value_or(false), "guard executes operation with a live handle");
    });
    entered.get_future().wait();

    std::atomic<bool> replaced{false};
    std::thread replacer([&]() {
        guard.Replace(second);
        replaced.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(!replaced.load(std::memory_order_acquire), "replace waits for active handle operation");

    release.set_value();
    reader.join();
    replacer.join();
    Expect(replaced.load(std::memory_order_acquire), "replace completes after active operation");
    Expect(closes.load(std::memory_order_relaxed) == 1, "replace closes previous handle once");
    guard.Close();
    Expect(closes.load(std::memory_order_relaxed) == 2, "close releases current handle once");
    Expect(!guard.WithHandle([](zhandle_t *) { return true; }).has_value(), "closed guard rejects operations");
}

static void TestPendingWorkEnforcesLimitAndTracksSends()
{
    PendingWork work;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> release{false};
    std::atomic<int> acquired{0};
    std::atomic<int> peak{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 32; ++i)
    {
        workers.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            if (!work.TryAcquireJob(4))
            {
                return;
            }
            acquired.fetch_add(1, std::memory_order_relaxed);
            int observed = work.Jobs();
            int old_peak = peak.load(std::memory_order_relaxed);
            while (observed > old_peak &&
                   !peak.compare_exchange_weak(old_peak, observed, std::memory_order_relaxed))
            {
            }
            while (!release.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            work.ReleaseJob();
        });
    }
    while (ready.load(std::memory_order_acquire) != 32)
    {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    while (acquired.load(std::memory_order_acquire) < 4)
    {
        std::this_thread::yield();
    }
    Expect(acquired.load(std::memory_order_relaxed) == 4, "only configured job slots are acquired");
    Expect(peak.load(std::memory_order_relaxed) <= 4, "concurrent jobs never exceed limit");

    work.BeginSend();
    release.store(true, std::memory_order_release);
    for (auto &worker : workers)
    {
        worker.join();
    }
    Expect(work.Jobs() == 0, "all job slots are released");
    Expect(work.Total() == 1, "queued response remains pending after business work");
    work.EndSend();
    Expect(work.Total() == 0, "send completion drains final pending work");
}

static void TestRetryAttemptStateMatrix()
{
    RetryAttemptState borrow_fail;
    Expect(borrow_fail.TryStartRetry(kRpcConnectFail, false),
           "borrow failure before send may retry once");

    RetryAttemptState closed_before_send;
    Expect(closed_before_send.TryStartRetry(kRpcConnectFail, false),
           "event-loop close before send may retry once");
    Expect(!closed_before_send.TryStartRetry(kRpcConnectFail, false),
           "only one pre-send retry is allowed");

    RetryAttemptState after_submit;
    Expect(!after_submit.TryStartRetry(kRpcConnectFail, true),
           "disconnect after submit is not replayed");

    RetryAttemptState timeout;
    Expect(!timeout.TryStartRetry(kRpcTimeout, false), "timeout is not replayed");

    RetryAttemptState overloaded;
    Expect(!overloaded.TryStartRetry(kRpcOverloaded, false), "server overload is not replayed");

    RetryAttemptState finish;
    std::atomic<int> winners{0};
    std::vector<std::thread> racers;
    for (int i = 0; i < 8; ++i)
    {
        racers.emplace_back([&]() {
            if (finish.TryFinish())
            {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &racer : racers)
    {
        racer.join();
    }
    Expect(winners.load(std::memory_order_relaxed) == 1, "final completion has one winner");
}

int main()
{
    TestRpcFrameFits();
    TestEncodeDecode();
    TestCircuitHalfOpen();
    TestPendingCompleteOnce();
    TestStructuredControllerError();
    TestShutdownDrainState();
    TestSafeRetryPolicy();
    TestRpcMetrics();
    TestHashWriteSerialized();
    TestMpmc();
    TestZkHandleGuardSerializesReplacement();
    TestPendingWorkEnforcesLimitAndTracksSends();
    TestRetryAttemptStateMatrix();
    if (g_failed != 0)
    {
        std::cerr << g_failed << " assertion(s) failed" << std::endl;
        return 1;
    }
    std::cout << "krpc_tests ok" << std::endl;
    return 0;
}
