# Krpc 后续优化实施计划

阶段 1～6 已经落地（同步 `wait_for`、每 loop tick、TLS 对象池、ZK 去 N+1、keepalive、阈值 `MSG_ZEROCOPY`）。  
阶段 7 只做了**发送前失败换节点**；滑动窗口熔断和 `SetFallback` 明确不做。  
就业向补强（CI、结构化错误码、graceful drain、指标、演示脚本）见 [README.md](README.md)。

已经更早完成的部分（无锁连接池、COW 哈希环、Muduo 异步客户端、用户态少拷贝、超时熔断、多 EventLoop）见 [README.md](README.md)。

原则：每一阶段单独能编译、能压测对比；不要把内核零拷贝和对象池揉进同一次提交。

---

## 优先级总表

| 顺序 | 项 | 主要收益 | 建议工期 | 状态 |
|------|----|----------|----------|------|
| 阶段 1 | 同步超时改 `wait_for`，拿掉每笔 `runAfter` | 压测热路径减 TimerQueue | 0.5 天 | 已落地 |
| 阶段 2 | 每 EventLoop 一个 tick，覆盖异步超时 | 异步也有超时，仍无 per-RPC 定时器 | 0.5～1 天 | 已落地 |
| 阶段 3 | 服务端 TLS 对象池 | 空业务少 `new/delete` | 1 天 | 已落地 |
| 阶段 4 | ZK `GetChildren` 去掉 N+1 次 `GetData` | Watcher 更新从 N+1 次变成 1 次 | 0.5 天 | 已落地 |
| 阶段 5 | TCP keepalive + 空闲连接剔除 | 避免借到僵尸连接 | 0.5 天 | 已落地 |
| 阶段 6 | **内核零拷贝（阈值路径）** | 大包减少内核拷贝；小包走原路径 | 1～2 天 | 已落地 |
| 阶段 7 | 异步失败换节点 / 滑动窗口熔断 | 功能完整（可选） | 1 天 | 部分完成：仅发送前失败换节点 |

## 实施记录

本节原先是施工说明。阶段 1～6 已落地并压测过；下面先写**完成对照、实测结果、踩过的坑**，后面仍保留原设计文字，方便对照代码。

更早已经有的能力（无锁连接池、COW 哈希环、Muduo 异步客户端、用户态少拷贝、连续失败熔断、多 EventLoop）见 [README.md](README.md)。

原则：每一阶段单独能编译、能压测对比；不要把内核零拷贝和对象池揉进同一次提交。

---

## 完成对照（2026-08-18）

| 顺序 | 项 | 状态 | 实际改动落点 |
|------|----|------|----------------|
| 阶段 1 | 同步超时改 `wait_for`，拿掉每笔 `runAfter` | **已完成** | `RpcPendingCall.h` `Wait(timeout_ms)`；`Krpcchannel.cc` 同步路径不再 `runAfter`；超时后 `queueInLoop` → `ApplyRpcFinish` |
| 阶段 2 | 每 EventLoop 一个 tick 做异步超时 | **已完成** | 新建 `TimeoutWheel.cc`；`KrpcClientIo.cc` `runEvery(0.01)`；只有 `done != nullptr` 才 `Register` |
| 阶段 3 | 服务端 TLS 对象池 | **已完成** | 新建 `RpcObjectPool.h/.cc`；`Krpcprovider.cc` IO 只拷 args，业务线程借对象再 `ParseFromArray` |
| 阶段 4 | ZK `GetChildren` 去掉 N+1 次 `GetData` | **已完成** | `zookeeperutil.cc` 直接 `push_back(nodes.data[i])` |
| 阶段 5 | TCP keepalive + 空闲连接剔除 | **已完成** | `TcpSockUtil.cc` 设 keepalive；`ConnContext.last_idle_ms`；`BorrowConnection` 超时空闲则 `forceClose` |
| 阶段 6 | 阈值 `MSG_ZEROCOPY` | **已完成（有降级）** | 新建 `ZeroCopySend.cc`；请求/响应 ≥ 16KB 才走；Login 小包不走 |
| 阶段 7 | 异步换节点 / 滑动窗口熔断 | **部分完成** | 发送前 `kRpcConnectFail` 会换一个节点；滑动窗口熔断和 `SetFallback` 不做 |

新增文件：`TimeoutWheel`、`RpcObjectPool`、`ConnContext`、`TcpSockUtil`、`ZeroCopySend`。  
配置新增：`tcp_keepalive_idle_s`、`conn_idle_evict_ms`、`zerocopy_threshold`、`enable_zerocopy`（见 `bin/test.conf`）。  
大包验证：`user.proto` 增加 `EchoBlob`，压测结束会发一笔 32KB。

---

## 压测结果（本机 2 核虚机）

| 场景 | 结果 |
|------|------|
| Login 小包，同步，10 万次 | 成功 100000，失败 0，QPS 约 5000 |
| EchoBlob 32KB，`enable_zerocopy=1` | 回显成功，`echo_size=32768` |
| 零拷贝计数 | `send=1 copied=1 complete=1`（内核走了 ZC 接口，但这一次仍拷了页） |

Login 不会碰到零拷贝路径（包远小于 16KB），所以小包正确率和 QPS 不受 ZC 拖累。

---

## 实施和压测里出现的问题

### 1. Muduo 没有公开 sockfd（阶段 5 / 6）

`TcpConnection` 头文件里没有 `fd()`。keepalive 和 `SO_ZEROCOPY` 都要用套接字。

**做法：** 建连成功时按本端/对端地址扫 `/proc/self/fd` 查一次，缓存进 `ConnContext.fd`。这不是热路径，只在 `OnConnection` 做。

### 2. `MSG_ZEROCOPY` 完成通知会把 EventLoop 打爆（阶段 6，实测踩中）

内核用 `MSG_ERRQUEUE` 通知「这块缓冲用完了」。这个通知会让 epoll 报 `EPOLLERR`。

Muduo 的 `TcpConnection::handleError` **只读 `SO_ERROR`，不抽 error queue**。于是：

- 日志狂刷 `TcpConnection::handleError ... SO_ERROR = 0 Success`
- error queue 不空，`EPOLLERR` 一直在，EventLoop 空转

第一次 EchoBlob 压测就是这样：Login 10 万次完全干净，一发 32KB 大包就开始刷几百行错误日志。RPC 其实成功了，但日志和 CPU 都不正常。

原计划只靠 10ms tick 抽 ERRQUEUE，**太晚了**：通知往往在 `send()` 返回后立刻就到。

**已改：** `TrySend` 成功后立刻 `DrainFd`（非阻塞 `recvmsg(MSG_ERRQUEUE)`）。抽空后 Muduo 就看不到 `EPOLLERR`。10ms tick 仍保留，给「发送完成晚于 RPC 回包」的情况用。复测后 `handleError` 条数为 0。

### 3. 虚机上内核仍可能拷贝（阶段 6，不是崩溃）

EchoBlob 计数是 `copied=1`。这是 `SO_EE_CODE_ZEROCOPY_COPIED`：页没锁住、或缓冲不满足零拷贝条件时，内核自己拷一份，但完成通知照样要等，缓冲不能提前回收。

路径是通的，**不等于** 2 核虚机上 `copy_to_iter` 一定下降。计划里写过：虚机以功能正确 + 计数为准。

### 4. 服务端 args 仍有一次 `string` 拷贝（阶段 3，有意简化）

IO 线程必须先把 Muduo buffer 里的参数拷走，才能 `retrieve`。现在用 `std::string args` 投递给业务线程，没有做 args 的 string 池。省下的是 Protobuf `Message` 和 `KrpcClosure` 的反复 `new/delete`。

### 5. 重启 server 时端口占用（测试操作，不是代码逻辑）

旧 `server` 进程还在时再起一次，会 `FATAL Address already in use`。需要先杀掉占用 `8000` 的进程。

### 6. 还没做、文档里仍算缺口

- 阶段 7：滑动窗口失败率熔断和 `SetFallback` 不做；发送前连接失败会换一个节点
- 大包连接仍和 Muduo 共用 fd；更干净的自管 socket 暂缓
- 没有单独做「服务端卡住看超时」和「双实例 ZK Watcher」的自动化用例（可用 `scripts/demo.sh` 人工演示）
- 没有 ASan 版测对端 RST
- 长期更干净的做法仍是：大包连接自管 socket，不和 Muduo `outputBuffer` 共用 fd

---

## 阶段 1：同步调用不要每笔 `runAfter`

### 现状

`KrpcChannel::IssueOnce` 每次都：

1. `loop->runAfter(timeout_sec, ...)` 往 Muduo `TimerQueue` 插一个节点  
2. 回包后再 `cancel`

10 万次同步压测 = 10 万次插入 + 10 万次取消，和 `send`/`OnMessage` 抢同一条 EventLoop。

同步调用的线程本来就在 `RpcPendingCall::Wait()` 上阻塞，超时完全可以在调用线程用 `wait_for` 完成。

### 改哪些文件

- `src/include/RpcPendingCall.h`：`Wait()` 改为带超时；成功返回 `true`，超时返回 `false`
- `src/Krpcchannel.cc`：`done == nullptr` 时 **不再** `runAfter`；`Wait(timeout)` 失败则 `queueInLoop` 里 `FinishRpcCall(..., "rpc timeout")`
- `src/RpcCall.cc`：保持 `TryComplete`，防止 IO 线程回包和调用线程超时同时完成

### 实施步骤

1. `Wait(int timeout_ms)` 内部 `cv.wait_for`，谓词仍是 `completed`
2. 若超时且 `TryComplete` 成功：`pending->loop->queueInLoop` 里关连接 / 归还（**禁止在业务线程直接操作 `TcpConnection`**）
3. 若 IO 线程已经 `TryComplete` 成功，调用线程 `wait_for` 被唤醒，走正常成功路径
4. 同步路径删除 `has_timer` / `runAfter` / `cancel`

### 验收

- 空业务同步压测：QPS 不低于改前，EventLoop 上 `TimerQueue` 不再随 QPS 线性增长（可用 `perf` 或日志计数）
- 把 `rpc_timeout_ms` 调成 1，服务端人为卡住：客户端应在约 1s 失败，连接标坏，进程不卡死
- 正常回包与超时并发：只会完成一次（`TryComplete`）

---

## 阶段 2：异步超时用「每 loop 一个 tick」

传了 `done` 的调用线程不会等 `cv`，必须在 EventLoop 上超时。不要再为每个异步请求 `runAfter`。

### 做法

每个客户端 EventLoop 启动时：

```text
runEvery(0.01, ScanTimeouts)   // 10ms 一跳，超时精度够用
```

`RpcPendingCall` 只存 `deadline_ms`（`steady_clock`），不存 `TimerId`。

本 loop 维护一个「进行中」链表 / `vector<weak_ptr<RpcPendingCall>>`，**只在该 loop 线程增删**。

`ScanTimeouts`：扫表，`now >= deadline && TryComplete` → 当超时处理。

当前一条连接同时只有 1 个 RPC，表的大小 = 占用中的连接数，扫描很便宜。

### 改哪些文件

- `src/KrpcClientIo.cc`：`startLoop` 的 `ThreadInitCallback` 里注册 `runEvery`
- `src/include/RpcPendingCall.h`：加 `deadline_ms`，去掉同步路径对 `TimerId` 的依赖
- `src/Krpcchannel.cc`：异步才登记到该 loop 的 pending 表
- 新建 `src/TimeoutWheel.cc`（或放进 `RpcCall.cc`）：`Register` / `Unregister` / `Scan`

### 验收

- 异步 `done` 回调：服务端卡住时，大约 `rpc_timeout_ms + 一个 tick` 内调用 `done`
- 成功回包后从链表摘掉，tick 扫不到已完成的 call
- 同步压测仍不走这张表（阶段 1 已用 `wait_for`）

---

## 阶段 3：服务端对象池

### 现状

`KrpcProvider::OnMessage` 每个请求：

- `GetRequestPrototype(method).New()`
- `GetResponsePrototype(method).New()`
- `new KrpcClosure`
- `SendRpcResponse` 里 `delete`

空 Login 时这三对分配往往比业务本身还贵。

### 做法（先做 TLS 池，不做跨线程无锁池）

按 **消息类型** 分池，存在业务线程的 `thread_local` 里：

```text
thread_local map<Descriptor*, vector<Message*>> req_pool;
thread_local map<Descriptor*, vector<Message*>> resp_pool;
thread_local vector<KrpcClosure*> closure_pool;
```

- 取：有则 pop，无则 `New()`
- 还：`Clear()` 后 push；池超过上限（例如每类型 64 个）则 `delete`
- `KrpcClosure` 改成可 `Reset(callback)`，不要每次 `new` 完在 `Run()` 里自杀

IO 线程拆包后仍把对象交给业务线程；**归还必须在同一业务线程**（TLS 池的前提）。现在 `ThreadPool` 不保证同一请求来回同一 worker，因此有两种选法：

1. **推荐**：池放在 `SendRpcResponse` 所在线程（业务线程 `done->Run()` 的线程）。取出也在 `m_thread_pool.run` 的 lambda **开头** 做 `New`/pop，不要在 IO 线程 `New`。  
   即：IO 只负责把 `raw bytes + method*` 投递；业务线程里再借对象、`ParseFromArray`、调用、还对象。
2. 若坚持 IO 线程解析：用带锁的全局池，热路径更差，不推荐。

阶段 3 配套调整：把 `ParseFromArray` 从 IO 挪到业务线程（多一次投递的是指针+长度，或把 args 拷进可复用 buffer）。为少一次业务侧拷贝，可对 args 用 `string` 池或固定 slab。

### 改哪些文件

- `src/Krpcprovider.cc` / `src/include/Krpcprovider.h`
- 新建 `src/include/RpcObjectPool.h`

### 验收

- 空业务压测：分配器采样（`perf` / jemalloc stats）显示 `New`/`delete` 次数远小于请求数
- 功能：Login 仍 100% 成功
- 池耗尽、异常解析失败时无泄漏（失败路径也要还或 delete）

---

## 阶段 4：ZK GetChildren 去掉 N+1

### 现状

`ZkClient::GetChildren`：`zoo_wget_children` 之后对每个子节点再 `zoo_get`。

服务端注册路径为：

```text
/UserServiceRpc/127.0.0.1:8000
```

**子节点名字已经是 `ip:port`**，节点 data 里又写了一遍同样的字符串。Watcher 更新时 N 个实例会打 N+1 次 ZK。

### 做法

`GetChildren` 成功后：

```text
vec.push_back(nodes.data[i]);   // 直接用子节点名
```

不再调用 `GetData`。若以后要在 data 里放权重、机房等信息，再改成「名字是地址、data 是 JSON」，那时才恢复按需 `get`。

### 改哪些文件

- `src/zookeeperutil.cc`：`GetChildren`
- `src/include/zookeeperutil.h`：注释说明节点名即地址

### 验收

- 启动仍能发现 1 个节点并完成 RPC
- 再起一个 server 实例（不同端口）：Watcher 更新哈希环，无需 N 次 `zoo_get`
- 抓 ZK 流量或打日志：一次 children 事件对应一次 `wget_children`

---

## 阶段 5：连接健康检查 / keepalive

### 目的

空闲很久后对端可能已经没了，池里还握着 fd，下次 RPC 才超时。这不提 QPS，但避免「借到僵尸连接」。

### 做法（两层）

**内核 TCP keepalive**（建连成功时设一次）：

- `SO_KEEPALIVE = 1`
- `TCP_KEEPIDLE = 30`
- `TCP_KEEPINTVL = 10`
- `TCP_KEEPCNT = 3`

客户端：`KrpcConnectPool::OnConnection` 里 `connected()==true` 时设置。  
服务端：`KrpcProvider::OnConnection` 里对 Muduo `TcpConnection` 设（Muduo 有 `setTcpNoDelay`，keepalive 用 `setsockopt` 取 fd，或检查当前 Muduo 是否已有封装）。

**应用层空闲剔除（可选，建议一起做）：**

- 连接归还时记下 `last_idle_ms`
- 借出时若空闲超过 60s，`forceClose`，再借下一条 / 新建
- 不做应用心跳协议（当前 RPC 没有 ping 帧），先靠 TCP keepalive

配置项（写入 `bin/test.conf` 和 README）：

```text
tcp_keepalive_idle_s=30
conn_idle_evict_ms=60000
```

### 改哪些文件

- `src/KrpcConnectPool.cc`
- `src/Krpcprovider.cc`
- `src/Krpcapplication.cc`：读配置
- `bin/test.conf`

### 验收

- `ss -nto` 能看到 keepalive 参数
- 杀掉 server 后，客户端下一次 RPC 应较快失败并熔断，而不是一直堵到 `rpc_timeout_ms`（视内核探测时间而定）

---

## 阶段 6：内核零拷贝

### 为什么要做，以及为什么不能「所有 RPC 都走零拷贝」

用户态已经少做了几次 `string` 拼包。内核零拷贝解决的是 **内核把用户缓冲拷进 socket 发送缓冲** 这一下。

当前 Login 请求只有几十字节：

- `MSG_ZEROCOPY` 要 pin 页、走 error queue 收完成通知、缓冲必须等内核用完才能改
- 小包上这些成本 **大于** 一次 `memcpy`

因此实现必须是 **阈值开关**：默认小包仍 `send()`；超过阈值才走内核零拷贝。这样简历和实现都站得住：大包有路径，小包不减速。

### 技术方案对比（只落地一种）

| 方案 | 适用 | 与本项目 |
|------|------|----------|
| `sendfile` | 磁盘文件 → socket | Protobuf 在内存里，**不适用** |
| `splice` / `vmsplice` | 管道中转 | 要实现 pipe 池，和 Muduo 输出队列冲突大，**不作为第一版** |
| **`MSG_ZEROCOPY`** | 内存缓冲 → TCP | Linux ≥ 4.14，适合 **≥ 阈值的内存包**，作为本阶段方案 |

第一版只做 **`setsockopt(SO_ZEROCOPY)` + `send(MSG_ZEROCOPY)` + 读 `MSG_ERRQUEUE`**。

### 与 Muduo 的关系（关键约束）

Muduo `TcpConnection::send` 会把数据拷进自己的 `outputBuffer_`，再在 loop 里 `write`。这条路径 **无法** 零拷贝。

必须增加一条旁路，且只在 EventLoop 线程调用：

1. 若 `payload.size() < zerocopy_threshold`：仍 `conn->send(...)`（现有逻辑）
2. 否则：
   - 把 payload 放到 **引用计数缓冲**（`shared_ptr<vector<char>>` 或 slab）
   - `::send(fd, data, len, MSG_NOSIGNAL | MSG_ZEROCOPY)`
   - 若返回 `EAGAIN`，不能把这块缓冲回收；挂到该连接的待完成队列，等 `EPOLLOUT` 再发
   - 套接字设 `SO_ZEROCOPY`
   - 在该连接的 Channel 上监听 `EPOLLERR`，`recvmsg(MSG_ERRQUEUE)` 收 `SO_EE_ORIGIN_ZEROCOPY`
     - `SO_EE_CODE_ZEROCOPY_COPIED`：内核还是拷了（页锁定失败等），算降级，仍要等通知才能释放缓冲
     - 完成通知带 `ee_info`～`ee_data` 序号，释放对应序号的缓冲

Muduo 默认 Channel 不读 error queue。需要：

- 要么给该 fd 再包一层自己的 `Channel`（和 Muduo `TcpConnection` 抢 fd，**禁止**）
- 要么 **大包连接不用 Muduo send**，用「Muduo 只负责 epoll 可读；可写/出错自己处理」——会拆连接模型，风险高
- **推荐第一版范围缩小**：只在 **客户端发送大请求** 或 **服务端发送大响应** 上，对 **已经持有的 sockfd** 做一次性 `send(MSG_ZEROCOPY)`，且要求：
  - 当前连接 **无 Muduo 未写完的 outputBuffer**（`outputBuffer()->readableBytes()==0`）
  - 一条连接一次只发一包（现有 one-in-flight 已满足）
  - 用 `conn->getLoop()->runInLoop` 保证在 IO 线程
  - 完成通知：在该 `TcpConnection` 上 `setErrorCallback` 若无此 API，则对 fd `dup` 不可行；改为 **loop 里 `runEvery(1ms)` 对大包在途连接 `recvmsg(ERRQUEUE)` 非阻塞 drain**（丑但和 Muduo 共存）

更干净的长期做法：大包连接改成自管 socket（不进 Muduo `outputBuffer`）。第一版为了能交差、可测，采用：

**「阈值以上、output 为空、one-in-flight → MSG_ZEROCOPY + 每 loop 随 tick 抽空 ERRQUEUE」**

阶段 2 的 10ms tick 可以顺便 drain error queue，不必再加 1ms 定时器。

### 缓冲生命周期

```text
struct ZcSendRec {
  uint32_t first_seq;
  uint32_t last_seq;          // send 返回后内核给的序号范围
  shared_ptr<vector<char>> buf;
};

连接上：queue<ZcSendRec> inflight_zc;
全局/每连接：atomic seq
```

`FinishRpcCall` **不能**在 `send` 返回后立刻复用/释放这块 `vector`。必须等 ERRQUEUE 对应序号。  
RPC 业务完成（对端已回包）通常晚于或早于内核发送完成：以 **两者都结束** 才能把连接还回池。否则会出现「连接已借给下一笔，缓冲却被内核还在 DMA」。

规则：

- 还连接条件：`rpc_completed && zc_inflight.empty()`
- 若 RPC 先完成、ZC 未完成：连接进入 **zc-wait** 状态，暂不入 idle 队列
- 若 ZC 先完成、RPC 未完成：维持现有 pending，逻辑不变

### 阈值与配置

```text
# 小于该字节数走普通 send（建议默认 8192 或 16384）
zerocopy_threshold=16384
# 0 表示关闭内核零拷贝
enable_zerocopy=1
```

内核：`uname` 检查；`setsockopt(SO_ZEROCOPY)` 失败则打日志并永久降级为普通 send。

### 改哪些文件

- `src/include/ZeroCopySend.h` / `src/ZeroCopySend.cc`：封装 `enable`、`send`、`drainErrQueue`、序号与缓冲表
- `src/KrpcConnectPool.cc`：建连 `SO_ZEROCOPY`；连接上下文挂 `ZcState`
- `src/Krpcchannel.cc`：请求体 ≥ 阈值走 ZC send
- `src/Krpcprovider.cc`：响应体 ≥ 阈值走 ZC send
- `src/KrpcClientIo.cc`：tick 里对本 loop 连接 `drainErrQueue`
- `bin/test.conf`、`README.md`

### 怎么测（必须分小包 / 大包）

1. **小包回归**：现有 Login 压测，阈值 16KB，QPS 不低于改前（ZC 路径不应被走到）
2. **大包功能**：临时加一个 `EchoBlob` 方法，body 32KB～1MB，`enable_zerocopy=1`  
   - 正确回显  
   - 日志或计数：`zc_send`、`zc_copied`（内核降级拷贝）、`zc_complete`
3. **对比**：同一 EchoBlob，关 ZC vs 开 ZC，看 CPU `softirq` / `perf` 里 `copy_to_iter` 是否下降（虚机 2 核可能不明显，以功能正确 + 计数为准）
4. **失败**：对端 RST 时缓冲仍被释放，无 Use-After-Free（ASan 编一版测）

### 明确不做

- `sendfile`（不是文件 RPC）
- 第一版 `splice` 管道池
- 小包强制 ZC
- 和 Muduo `outputBuffer` 同时写同一 fd 的混用（output 非空则拒绝 ZC，改走普通 send）

---

## 阶段 7（可选）：熔断与降级补全

- 异步 `done` 失败后再换一个节点（现在只有同步会试）
- 熔断改为时间窗失败率，而不是只看连续失败次数
- 业务 Fallback 回调：当时约定第一期不做，需要时再加 `SetFallback`

不阻塞阶段 1～6。

---

## 建议 Git / 提交方式

每个阶段一次提交，方便回滚和写简历：

1. `timeout: sync wait_for, drop per-rpc runAfter`
2. `timeout: per-loop tick for async`
3. `provider: thread-local message/closure pool`
4. `zk: use child name as address, drop N+1 get`
5. `conn: tcp keepalive and idle evict`
6. `net: MSG_ZEROCOPY for payloads >= threshold`

---

## 验收总清单

改完后至少保留两种压测：

| 场景 | 期望 |
|------|------|
| Login 小包、同步、现有并发 | 正确率 100%；QPS 不因 ZC/对象池回退 |
| 服务端卡住 | 同步在 `rpc_timeout_ms` 内返回；连接可被熔断 |
| ZK 增删实例 | 一次 children watch，无 N 次 `zoo_get` |
| EchoBlob ≥ 阈值 | ZC 计数增加；关 ZC 仍正确 |

---

## 当前代码锚点

| 点 | 文件（落地后） |
|----|----------------|
| 同步 `wait_for` / 异步 deadline | `src/include/RpcPendingCall.h` |
| 同步超时收尾（IO 线程） | `src/RpcCall.cc` `ApplyRpcFinish`；`src/Krpcchannel.cc` `IssueOnce` |
| 异步超时 tick | `src/TimeoutWheel.cc`；`src/KrpcClientIo.cc` `runEvery(0.01)` |
| 服务端对象池 | `src/RpcObjectPool.cc`；`src/Krpcprovider.cc` `OnMessage` / `SendRpcResponse` |
| ZK 子节点名即地址 | `src/zookeeperutil.cc` `GetChildren` |
| 建连 keepalive / 空闲剔除 | `src/TcpSockUtil.cc`；`src/KrpcConnectPool.cc` `OnConnection` / `BorrowConnection` |
| 连接上下文（fd、pending、ZC 在途） | `src/include/ConnContext.h` |
| 大包 `MSG_ZEROCOPY` + 立刻抽 ERRQUEUE | `src/ZeroCopySend.cc` `TrySend` / `DrainFd` |
| 配置 | `bin/test.conf` `src/Krpcapplication.cc` |
