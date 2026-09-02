#ifndef CONSISTENT_HASH_H
#define CONSISTENT_HASH_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct HashConfig
{
    int replicas = 10;
    int min_replicas = 5;
    int max_replicas = 200;
    double balance_threshold = 0.25;
};

// 不可变哈希环快照：读路径 atomic_load 后无锁二分。
struct HashRingSnapshot
{
    std::vector<uint32_t> keys;
    std::unordered_map<uint32_t, std::string> ring;
    std::unordered_map<std::string, int> node_replicas;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<long long>>> node_counts;
};

class ConsistentHash
{
public:
    explicit ConsistentHash(HashConfig cfg = HashConfig{});
    ~ConsistentHash();

    void AddNodes(const std::vector<std::string> &nodes);
    void RemoveNode(const std::string &node);
    void UpdateNodes(const std::vector<std::string> &nodes);

    // exclude 非空时跳过该物理节点，用于熔断后换节点。
    std::string GetTargetNode(const std::string &key, const std::string &exclude = "");

    std::unordered_map<std::string, double> GetStats();

private:
    uint32_t hash_func(const std::string &data) const;
    void fill_node(HashRingSnapshot &snap, const std::string &node, int replicas) const;
    void publish(std::shared_ptr<const HashRingSnapshot> snap);
    std::shared_ptr<const HashRingSnapshot> load() const;

    HashConfig m_config;
    std::shared_ptr<const HashRingSnapshot> m_snapshot;
    std::mutex m_write_mu;
    std::atomic<long long> m_total_requests{0};
};

#endif
