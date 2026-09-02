#include "RpcMetrics.h"

#include "Krpcapplication.h"
#include "KrpcLogger.h"
#include "RpcError.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
constexpr std::array<uint64_t, RpcMetricsSnapshot::kBucketCount> kLatencyUpperBoundsUs{
    100, 500, 1000, 5000, 10000, 50000, 100000, 500000, 5000000};
}

uint64_t RpcMetricsSnapshot::ApproxPercentileUs(double percentile) const
{
    uint64_t sample_count = 0;
    for (const uint64_t count : latency_buckets)
    {
        sample_count += count;
    }
    if (sample_count == 0)
    {
        return 0;
    }

    const double clamped = std::max(0.0, std::min(1.0, percentile));
    const uint64_t rank = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(clamped * static_cast<double>(sample_count))));
    uint64_t seen = 0;
    for (size_t i = 0; i < latency_buckets.size(); ++i)
    {
        seen += latency_buckets[i];
        if (seen >= rank)
        {
            return kLatencyUpperBoundsUs[i];
        }
    }
    return kLatencyUpperBoundsUs.back();
}

std::string RpcMetricsSnapshot::ToString() const
{
    std::ostringstream out;
    out << "rpc_metrics total=" << total
        << " success=" << success
        << " timeout=" << timeout
        << " connect_fail=" << connect_fail
        << " circuit_open=" << circuit_open
        << " overloaded=" << overloaded
        << " other_error=" << other_error
        << " inflight=" << inflight
        << " p50_us<=" << ApproxPercentileUs(0.50)
        << " p95_us<=" << ApproxPercentileUs(0.95)
        << " p99_us<=" << ApproxPercentileUs(0.99);
    return out.str();
}

RpcMetrics &RpcMetrics::Instance()
{
    static RpcMetrics metrics;
    return metrics;
}

void RpcMetrics::RecordStarted()
{
    total_.fetch_add(1, std::memory_order_relaxed);
    inflight_.fetch_add(1, std::memory_order_relaxed);
}

void RpcMetrics::RecordFinished(int error_code, uint64_t latency_us)
{
    inflight_.fetch_sub(1, std::memory_order_relaxed);
    switch (error_code)
    {
    case kRpcOk:
        success_.fetch_add(1, std::memory_order_relaxed);
        break;
    case kRpcTimeout:
        timeout_.fetch_add(1, std::memory_order_relaxed);
        break;
    case kRpcConnectFail:
        connect_fail_.fetch_add(1, std::memory_order_relaxed);
        break;
    case kRpcCircuitOpen:
        circuit_open_.fetch_add(1, std::memory_order_relaxed);
        break;
    case kRpcOverloaded:
        overloaded_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        other_error_.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    size_t bucket = 0;
    while (bucket + 1 < kLatencyUpperBoundsUs.size() &&
           latency_us > kLatencyUpperBoundsUs[bucket])
    {
        ++bucket;
    }
    latency_buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
}

RpcMetricsSnapshot RpcMetrics::Snapshot() const
{
    RpcMetricsSnapshot snapshot;
    snapshot.total = total_.load(std::memory_order_relaxed);
    snapshot.success = success_.load(std::memory_order_relaxed);
    snapshot.timeout = timeout_.load(std::memory_order_relaxed);
    snapshot.connect_fail = connect_fail_.load(std::memory_order_relaxed);
    snapshot.circuit_open = circuit_open_.load(std::memory_order_relaxed);
    snapshot.overloaded = overloaded_.load(std::memory_order_relaxed);
    snapshot.other_error = other_error_.load(std::memory_order_relaxed);
    snapshot.inflight = inflight_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < snapshot.latency_buckets.size(); ++i)
    {
        snapshot.latency_buckets[i] = latency_buckets_[i].load(std::memory_order_relaxed);
    }
    return snapshot;
}

void RpcMetrics::Dump(const char *where) const
{
    LOG(INFO) << (where != nullptr ? where : "rpc") << " " << Snapshot().ToString();
}

void MaybeRpcAccessLog(const char *side,
                       uint64_t request_id,
                       const std::string &service,
                       const std::string &method,
                       const std::string &node,
                       int error_code,
                       uint64_t latency_us)
{
    if (!KrpcApplication::EnableAccessLog())
    {
        return;
    }
    LOG(INFO) << "rpc_access side=" << (side != nullptr ? side : "")
              << " request_id=" << request_id
              << " service=" << service
              << " method=" << method
              << " node=" << node
              << " error_code=" << error_code
              << " latency_us=" << latency_us;
}
