#ifndef _zookeeperutil_h_
#define _zookeeperutil_h_

#include "ZkHandleGuard.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ZkClient
{
public:
    using ReconnectedFn = std::function<void()>;

    ZkClient();
    ~ZkClient();

    void Start();
    void Stop();
    void SetReconnectedCallback(ReconnectedFn cb);

    // state: 0 永久节点，ZOO_EPHEMERAL 临时节点。会话恢复后会按记录重创建。
    void Create(const char *path, const char *data, int datalen, int state = 0);
    std::string GetData(const char *path);
    std::vector<std::string> GetChildren(const char *path, watcher_fn fn, void *cbContext);

private:
    struct NodeSpec
    {
        std::string path;
        std::string data;
        int state = 0;
    };

    struct WatchSpec
    {
        std::string path;
        watcher_fn fn = nullptr;
        void *ctx = nullptr;
    };

    static void SessionWatcher(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx);
    void OnSessionEvent(int state);
    void ReconnectLoop();
    void RecreateSession();
    void ReplayCreates();
    void ReplayWatches();

    std::string m_connstr;
    ZkHandleGuard m_handle;
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_connected = false;
    bool m_ever_connected = false;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_need_reconnect{false};
    std::thread m_reconnect_thread;
    ReconnectedFn m_on_reconnected;
    std::vector<NodeSpec> m_created;
    std::vector<WatchSpec> m_watches;
};

#endif
