#include "CircuitBreaker.h"

#include <chrono>

CircuitBreaker &CircuitBreaker::Instance()
{
    static CircuitBreaker inst;
    return inst;
}

void CircuitBreaker::Configure(int fail_threshold, int reset_ms)
{
    if (fail_threshold > 0)
    {
        fail_threshold_ = fail_threshold;
    }
    if (reset_ms > 0)
    {
        reset_ms_ = reset_ms;
    }
}

int64_t CircuitBreaker::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

CircuitBreaker::NodeState *CircuitBreaker::GetOrCreate(const std::string &node)
{
    thread_local std::string last_node;
    thread_local NodeState *last_state = nullptr;
    if (last_state != nullptr && last_node == node)
    {
        return last_state;
    }

    NodeState *state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = nodes_.find(node);
        if (it == nodes_.end())
        {
            state = new NodeState();
            nodes_[node] = state;
        }
        else
        {
            state = it->second;
        }
    }
    last_node = node;
    last_state = state;
    return state;
}

bool CircuitBreaker::AllowRequest(const std::string &node)
{
    NodeState *s = GetOrCreate(node);
    const State st = static_cast<State>(s->state.load(std::memory_order_acquire));
    if (st == State::Closed)
    {
        return true;
    }
    if (st == State::Open)
    {
        const int64_t now = NowMs();
        if (now < s->open_until_ms.load(std::memory_order_relaxed))
        {
            return false;
        }
        int expected = static_cast<int>(State::Open);
        if (s->state.compare_exchange_strong(expected, static_cast<int>(State::HalfOpen),
                                             std::memory_order_acq_rel))
        {
            s->half_open_inflight.store(1, std::memory_order_relaxed);
            return true;
        }
        // CAS 失败说明别人已经改了状态：只有已经回到 Closed 才放行。
        return static_cast<State>(s->state.load(std::memory_order_acquire)) == State::Closed;
    }

    // Half-Open：只放行 1 个探测请求
    const int prev = s->half_open_inflight.fetch_add(1, std::memory_order_acq_rel);
    if (prev >= 1)
    {
        s->half_open_inflight.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void CircuitBreaker::RecordSuccess(const std::string &node)
{
    NodeState *s = GetOrCreate(node);
    s->consecutive_failures.store(0, std::memory_order_relaxed);
    s->half_open_inflight.store(0, std::memory_order_relaxed);
    s->state.store(static_cast<int>(State::Closed), std::memory_order_release);
}

void CircuitBreaker::RecordFailure(const std::string &node)
{
    NodeState *s = GetOrCreate(node);
    const int fails = s->consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    const State st = static_cast<State>(s->state.load(std::memory_order_acquire));
    if (st == State::HalfOpen || fails >= fail_threshold_)
    {
        s->open_until_ms.store(NowMs() + reset_ms_, std::memory_order_relaxed);
        s->half_open_inflight.store(0, std::memory_order_relaxed);
        s->state.store(static_cast<int>(State::Open), std::memory_order_release);
    }
}
