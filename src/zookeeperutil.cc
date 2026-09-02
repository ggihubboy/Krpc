#include "zookeeperutil.h"
#include "Krpcapplication.h"
#include "KrpcLogger.h"

#include <chrono>

ZkClient::ZkClient() = default;

ZkClient::~ZkClient()
{
    Stop();
}

void ZkClient::SetReconnectedCallback(ReconnectedFn cb)
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_on_reconnected = std::move(cb);
}

void ZkClient::Start()
{
    std::string host = KrpcApplication::GetInstance().GetConfig().Load("zookeeperip");
    std::string port = KrpcApplication::GetInstance().GetConfig().Load("zookeeperport");
    m_connstr = host + ":" + port;

    m_zhandle = zookeeper_init(m_connstr.c_str(), &ZkClient::SessionWatcher, 6000, nullptr, this, 0);
    if (m_zhandle == nullptr)
    {
        LOG(ERROR) << "zookeeper_init error";
        exit(EXIT_FAILURE);
    }

    if (!m_reconnect_thread.joinable())
    {
        m_stop.store(false, std::memory_order_relaxed);
        m_reconnect_thread = std::thread([this]() { ReconnectLoop(); });
    }

    std::unique_lock<std::mutex> lock(m_mu);
    m_cv.wait(lock, [this] { return m_connected; });
    LOG(INFO) << "zookeeper_init success";
}

void ZkClient::Stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    m_need_reconnect.store(false, std::memory_order_relaxed);
    m_cv.notify_all();
    if (m_reconnect_thread.joinable())
    {
        m_reconnect_thread.join();
    }
    if (m_zhandle != nullptr)
    {
        zookeeper_close(m_zhandle);
        m_zhandle = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mu);
    m_connected = false;
}

void ZkClient::SessionWatcher(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx)
{
    (void)zh;
    (void)path;
    auto *self = static_cast<ZkClient *>(watcherCtx);
    if (self == nullptr)
    {
        return;
    }
    if (type == ZOO_SESSION_EVENT)
    {
        self->OnSessionEvent(state);
    }
}

void ZkClient::OnSessionEvent(int state)
{
    if (state == ZOO_CONNECTED_STATE)
    {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_connected = true;
            m_ever_connected = true;
        }
        m_cv.notify_all();
        return;
    }

    if (state == ZOO_EXPIRED_SESSION_STATE)
    {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_connected = false;
        }
        m_need_reconnect.store(true, std::memory_order_relaxed);
        m_cv.notify_all();
    }
}

void ZkClient::ReconnectLoop()
{
    while (!m_stop.load(std::memory_order_relaxed))
    {
        std::unique_lock<std::mutex> lock(m_mu);
        m_cv.wait(lock, [this] {
            return m_stop.load(std::memory_order_relaxed) || m_need_reconnect.load(std::memory_order_relaxed);
        });
        if (m_stop.load(std::memory_order_relaxed))
        {
            return;
        }
        m_need_reconnect.store(false, std::memory_order_relaxed);
        lock.unlock();
        RecreateSession();
    }
}

void ZkClient::RecreateSession()
{
    zhandle_t *old = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        old = m_zhandle;
        m_zhandle = nullptr;
        m_connected = false;
    }
    if (old != nullptr)
    {
        zookeeper_close(old);
    }
    zhandle_t *zh = zookeeper_init(m_connstr.c_str(), &ZkClient::SessionWatcher, 6000, nullptr, this, 0);
    if (zh == nullptr)
    {
        LOG(ERROR) << "zookeeper reconnect failed, will retry";
        m_need_reconnect.store(true, std::memory_order_relaxed);
        m_cv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_zhandle = zh;
    }

    {
        std::unique_lock<std::mutex> lock(m_mu);
        if (!m_cv.wait_for(lock, std::chrono::seconds(6), [this] { return m_connected; }))
        {
            LOG(ERROR) << "zookeeper reconnect wait timeout";
            m_need_reconnect.store(true, std::memory_order_relaxed);
            m_cv.notify_all();
            return;
        }
    }

    ReplayCreates();
    ReplayWatches();
    ReconnectedFn cb;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        cb = m_on_reconnected;
    }
    if (cb)
    {
        cb();
    }
}

void ZkClient::ReplayCreates()
{
    std::vector<NodeSpec> specs;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        specs = m_created;
    }
    for (const auto &spec : specs)
    {
        char path_buffer[128];
        int bufferlen = sizeof(path_buffer);
        const char *data = spec.data.empty() ? nullptr : spec.data.c_str();
        const int datalen = spec.data.empty() ? 0 : static_cast<int>(spec.data.size());
        int flag = zoo_exists(m_zhandle, spec.path.c_str(), 0, nullptr);
        if (flag == ZNONODE)
        {
            flag = zoo_create(m_zhandle, spec.path.c_str(), data, datalen, &ZOO_OPEN_ACL_UNSAFE, spec.state,
                              path_buffer, bufferlen);
            if (flag != ZOK && flag != ZNODEEXISTS)
            {
                LOG(ERROR) << "znode recreate failed path:" << spec.path;
            }
        }
    }
}

void ZkClient::ReplayWatches()
{
    std::vector<WatchSpec> watches;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        watches = m_watches;
    }
    for (const auto &w : watches)
    {
        struct String_vector nodes;
        const int flag = zoo_wget_children(m_zhandle, w.path.c_str(), w.fn, w.ctx, &nodes);
        if (flag == ZOK)
        {
            deallocate_String_vector(&nodes);
            if (w.fn)
            {
                w.fn(m_zhandle, ZOO_CHILD_EVENT, ZOO_CONNECTED_STATE, w.path.c_str(), w.ctx);
            }
        }
        else
        {
            LOG(ERROR) << "rewatch children failed path:" << w.path;
        }
    }
}

void ZkClient::Create(const char *path, const char *data, int datalen, int state)
{
    char path_buffer[128];
    int bufferlen = sizeof(path_buffer);

    {
        std::lock_guard<std::mutex> lock(m_mu);
        NodeSpec spec;
        spec.path = path ? path : "";
        if (data != nullptr && datalen > 0)
        {
            spec.data.assign(data, datalen);
        }
        spec.state = state;
        bool found = false;
        for (auto &old : m_created)
        {
            if (old.path == spec.path)
            {
                old = spec;
                found = true;
                break;
            }
        }
        if (!found)
        {
            m_created.push_back(spec);
        }
    }

    int flag = zoo_exists(m_zhandle, path, 0, nullptr);
    if (flag == ZNONODE)
    {
        flag = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
        if (flag == ZOK)
        {
            LOG(INFO) << "znode create success... path:" << path;
        }
        else
        {
            LOG(ERROR) << "znode create failed... path:" << path;
            exit(EXIT_FAILURE);
        }
    }
}

std::string ZkClient::GetData(const char *path)
{
    char buf[64] = {0};
    int bufferlen = sizeof(buf);

    int flag = zoo_get(m_zhandle, path, 0, buf, &bufferlen, nullptr);
    if (flag != ZOK)
    {
        LOG(ERROR) << "zoo_get error";
        return "";
    }
    return buf;
}

std::vector<std::string> ZkClient::GetChildren(const char *path, watcher_fn fn, void *cbContext)
{
    {
        std::lock_guard<std::mutex> lock(m_mu);
        WatchSpec spec;
        spec.path = path ? path : "";
        spec.fn = fn;
        spec.ctx = cbContext;
        bool found = false;
        for (auto &old : m_watches)
        {
            if (old.path == spec.path)
            {
                old = spec;
                found = true;
                break;
            }
        }
        if (!found)
        {
            m_watches.push_back(spec);
        }
    }

    struct String_vector nodes;
    int flag = zoo_wget_children(m_zhandle, path, fn, cbContext, &nodes);

    std::vector<std::string> vec;
    if (flag == ZOK)
    {
        for (int i = 0; i < nodes.count; ++i)
        {
            vec.push_back(nodes.data[i]);
        }
        deallocate_String_vector(&nodes);
    }
    else
    {
        LOG(ERROR) << "zoo_wget_children error, path: " << path;
    }
    return vec;
}
