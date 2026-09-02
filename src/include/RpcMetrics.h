#ifndef KRPC_RPC_METRICS_H
#define KRPC_RPC_METRICS_H

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

struct RpcMetricsSnapshot
{
    static constexpr size_t kBucketCount = 9;

    uint64_t total = 0;
    uint64_t success = 0;
    uint64_t timeout = 0;
    uint64_t connect_fail = 0;
    uint64_t circuit_open = 0;
    uint64_t overloaded = 0;
    uint64_t other_error = 0;
    uint64_t inflight = 0;
    std::array<uint64_t, kBucketCount> latency_buckets{};

    uint64_t ApproxPercentileUs(double percentile) const;
    std::string ToString() const;
};

// Process-local counters. Updates use only relaxed atomics and fixed buckets,
// so the RPC hot path performs no allocation or locking.
class RpcMetrics
{
public:
    static RpcMetrics &Instance();

    void RecordStarted();
    void RecordFinished(int error_code, uint64_t latency_us);
    RpcMetricsSnapshot Snapshot() const;
    void Dump(const char *where) const;

private:
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> success_{0};
    std::atomic<uint64_t> timeout_{0};
    std::atomic<uint64_t> connect_fail_{0};
    std::atomic<uint64_t> circuit_open_{0};
    std::atomic<uint64_t> overloaded_{0};
    std::atomic<uint64_t> other_error_{0};
    std::atomic<uint64_t> inflight_{0};
    std::array<std::atomic<uint64_t>, RpcMetricsSnapshot::kBucketCount> latency_buckets_{};
};

void MaybeRpcAccessLog(const char *side,
                       uint64_t request_id,
                       const std::string &service,
                       const std::string &method,
                       const std::string &node,
                       int error_code,
                       uint64_t latency_us);

#endif
