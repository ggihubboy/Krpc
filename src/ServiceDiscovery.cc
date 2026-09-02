#include "ServiceDiscovery.h"
#include "KrpcLogger.h"

#include <atomic>

ServiceDiscovery &ServiceDiscovery::GetInstance()
{
    static ServiceDiscovery instance;
    return instance;
}

void ServiceDiscovery::Init()
{
    m_zkClient.Start();
    auto empty = std::make_shared<DiscoverySnapshot>();
    std::atomic_store_explicit(&m_snapshot, std::shared_ptr<const DiscoverySnapshot>(std::move(empty)),
                               std::memory_order_relaxed);
    m_zkClient.SetReconnectedCallback([this]() {
        auto snap = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
        if (!snap)
        {
            return;
        }
        for (const auto &pair : snap->services)
        {
            const std::string path = "/" + pair.first;
            std::vector<std::string> nodes = m_zkClient.GetChildren(path.c_str(), WatcherCallback, this);
            if (pair.second)
            {
                pair.second->UpdateNodes(nodes);
            }
        }
    });
}

std::shared_ptr<ConsistentHash> ServiceDiscovery::EnsureServiceLocked(
    const std::string &service_name, const std::vector<std::string> &nodes)
{
    auto old_snap = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    if (old_snap)
    {
        auto it = old_snap->services.find(service_name);
        if (it != old_snap->services.end())
        {
            return it->second;
        }
    }

    auto hash_ring = std::make_shared<ConsistentHash>();
    hash_ring->UpdateNodes(nodes);

    auto next = std::make_shared<DiscoverySnapshot>();
    if (old_snap)
    {
        next->services = old_snap->services;
    }
    next->services[service_name] = hash_ring;
    std::atomic_store_explicit(&m_snapshot, std::shared_ptr<const DiscoverySnapshot>(next),
                               std::memory_order_release);
    return hash_ring;
}

std::string ServiceDiscovery::GetTargetNode(const std::string &service_name,
                                            const std::string &key,
                                            const std::string &exclude)
{
    auto snap = std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
    if (snap)
    {
        auto it = snap->services.find(service_name);
        if (it != snap->services.end() && it->second)
        {
            return it->second->GetTargetNode(key, exclude);
        }
    }

    const std::string path = "/" + service_name;
    std::vector<std::string> new_nodes = m_zkClient.GetChildren(path.c_str(), WatcherCallback, this);

    std::shared_ptr<ConsistentHash> hash_ring;
    {
        std::lock_guard<std::mutex> lock(m_init_mtx);
        hash_ring = EnsureServiceLocked(service_name, new_nodes);
    }
    return hash_ring->GetTargetNode(key, exclude);
}

void ServiceDiscovery::WatcherCallback(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx)
{
    (void)zh;
    (void)state;
    if (type != ZOO_CHILD_EVENT)
    {
        return;
    }

    auto *sd = static_cast<ServiceDiscovery *>(watcherCtx);
    const std::string path_str = path ? path : "";
    if (path_str.size() < 2)
    {
        return;
    }
    const std::string service_name = path_str.substr(1);

    std::vector<std::string> new_nodes = sd->m_zkClient.GetChildren(path_str.c_str(), WatcherCallback, sd);

    auto snap = std::atomic_load_explicit(&sd->m_snapshot, std::memory_order_acquire);
    if (snap)
    {
        auto it = snap->services.find(service_name);
        if (it != snap->services.end() && it->second)
        {
            it->second->UpdateNodes(new_nodes);
            return;
        }
    }

    std::lock_guard<std::mutex> lock(sd->m_init_mtx);
    sd->EnsureServiceLocked(service_name, new_nodes);
}
