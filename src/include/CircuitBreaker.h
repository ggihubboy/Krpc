#ifndef KRPC_CIRCUIT_BREAKER_H
#define KRPC_CIRCUIT_BREAKER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// 按 ip:port 隔离的熔断器。Closed / Open / Half-Open。
// 热路径只用 atomic，创建新节点时才加锁。
class CircuitBreaker
{
public:
    static CircuitBreaker &Instance();

    void Configure(int fail_threshold, int reset_ms);

    // true 表示允许发请求
    bool AllowRequest(const std::string &node);

    void RecordSuccess(const std::string &node);
    void RecordFailure(const std::string &node);

private:
    enum class State : int
    {
        Closed = 0,
        Open = 1,
        HalfOpen = 2
    };

    struct NodeState
    {
        std::atomic<int> state{static_cast<int>(State::Closed)};
        std::atomic<int> consecutive_failures{0};
        std::atomic<int> half_open_inflight{0};
        std::atomic<int64_t> open_until_ms{0};
    };

    CircuitBreaker() = default;

    NodeState *GetOrCreate(const std::string &node);
    static int64_t NowMs();

    int fail_threshold_ = 5;
    int reset_ms_ = 1000;

    std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<NodeState>> nodes_;
};

#endif
