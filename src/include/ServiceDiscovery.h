#ifndef SERVICE_DISCOVERY_H
#define SERVICE_DISCOVERY_H

#include "ConsistentHash.h"
#include "zookeeperutil.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct DiscoverySnapshot
{
    std::unordered_map<std::string, std::shared_ptr<ConsistentHash>> services;
};

class ServiceDiscovery
{
public:
    static ServiceDiscovery &GetInstance();
    void Init();
    std::string GetTargetNode(const std::string &service_name,
                              const std::string &key,
                              const std::string &exclude = "");

private:
    ServiceDiscovery() = default;
    ~ServiceDiscovery() = default;
    ServiceDiscovery(const ServiceDiscovery &) = delete;
    ServiceDiscovery &operator=(const ServiceDiscovery &) = delete;

    std::shared_ptr<ConsistentHash> EnsureServiceLocked(const std::string &service_name,
                                                        const std::vector<std::string> &nodes);

    ZkClient m_zkClient;
    std::shared_ptr<const DiscoverySnapshot> m_snapshot;
    std::mutex m_init_mtx;

    static void WatcherCallback(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx);
};

#endif
