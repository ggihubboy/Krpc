# Krpc

[![CI](https://github.com/ggihubboy/Krpc/actions/workflows/ci.yml/badge.svg)](https://github.com/ggihubboy/Krpc/actions/workflows/ci.yml)

C++ RPC 学习骨架：Muduo + Protobuf + ZooKeeper。  
**这是给校招/作品集用的学习版，不是生产框架。** 没有 TLS、没有鉴权；ZooKeeper ACL 仍是开放的。

当前版本相对上一轮补了协议校验、错误回包、连接内多路复用、熔断 Half-Open、ZK 会话恢复、过载保护和优雅退出。用 `bin/krpc_tests` 做回归，不要只拿空 Login 压测当正确性证明。

## 能做什么

- 用 Protobuf 定义服务，客户端像调本地函数一样调远程方法（同步阻塞或异步 `done`）
- 服务端 Muduo Reactor 收包，业务丢进有上限的线程池；请求/响应走线程本地对象池
- ZooKeeper 注册发现；临时节点在会话恢复后会重注册，客户端会重装 watch
- 一致性哈希选节点（读路径无锁快照）；写路径加锁，不再用后台线程改副本数
- 客户端连接池：一条 TCP 上可以同时挂多个 RPC（`request_id`），空闲连接走 MPMC 队列
- 同步超时在调用线程 `wait_for`；异步超时用每条 EventLoop 10ms 扫描
- 按节点熔断（Closed / Open / Half-Open，探测只放行 1 个）
- 包过大、长度溢出、header 损坏会关连接；方法不存在等业务错误会回错误帧
- 服务端队列过载直接拒绝；`SIGINT`/`SIGTERM` 退出时关掉 ZK（临时节点消失）

## 明确不做

- TLS / 鉴权 / 业务 Fallback
- 滑动窗口失败率熔断、异步失败自动换节点
- 完整的分布式追踪和直方图指标
- IPv6 字面量地址（`ip:port` 按最后一个 `:` 切开，仅按 IPv4 来用）

## 架构

```
客户端线程                    EventLoop 线程                 服务端
   |                              |                           |
 CallMethod                       |                      TcpServer IO
   | 编一帧（带 request_id）        |                           |
   | 无锁读哈希环快照选节点        |                           |
   | 熔断检查 / 必要时换节点       |                           |
   | 连接池借一条可发连接 -------->|  send / 大包 MSG_ZEROCOPY  |
   | 同步 Wait / 异步登记超时      |  10ms tick：超时 / ERRQUEUE|
   |                              |  按 request_id 对上回包     |
   | <---- Notify / done.Run -----|  inflight 降下来再可出借    |
   |                              |                      业务线程池（有上限）
```

## 协议

请求和响应都是同一套长度前缀帧：

```
total_len(4, 网络序) + header_len(4, 网络序) + header + payload
total_len = 4 + header_len + payload 长度（不含最前面 4 字节）
```

- 请求 header 是 `RpcHeader`（service / method / args_size / request_id），payload 是方法参数
- 响应 header 是 `RpcMeta`（request_id / error_code / error_msg），成功时 payload 是 protobuf 响应；失败时 payload 为空
- `total_len` 超过 `rpc_max_body_bytes`（默认 16MB）、`header_len` 越界、加法溢出，一律视为坏帧并**断开连接**

一条连接上可以同时有多个未完成请求，靠 `request_id` 对应。默认每条连接最多 `max_inflight_per_conn`（32）个在途 RPC。

## 配置

见 `bin/test.conf`：

| 项 | 含义 | 默认 |
|----|------|------|
| `rpcserverip` / `rpcserverport` | 服务监听地址 | 必填 |
| `zookeeperip` / `zookeeperport` | 注册中心 | 必填 |
| `rpc_timeout_ms` | 客户端 RPC 超时 | 3000 |
| `circuit_fail_threshold` | 连续失败多少次打开熔断 | 5 |
| `circuit_reset_ms` | 熔断打开后多久允许一次探测 | 1000 |
| `tcp_keepalive_idle_s` | TCP keepalive 空闲多久开始探测 | 30 |
| `conn_idle_evict_ms` | 空闲超过该毫秒且无在途 RPC 则关掉；`0` 关闭 | 60000 |
| `zerocopy_threshold` | 小于该字节数走普通 send | 16384 |
| `enable_zerocopy` | `1` 打开内核零拷贝；`0` 关闭 | 1 |
| `rpc_max_body_bytes` | 单帧上限，超出关连接 | 16777216 |
| `max_inflight_per_conn` | 每条连接同时未完成 RPC 上限 | 32 |
| `server_max_pending` | 服务端线程池排队上限，超出回过载错误 | 4096 |
| `server_shutdown_grace_ms` | 停止注册后等待在途请求完成的最长时间 | 5000 |

## 客户端

```cpp
KrpcApplication::Init(argc, argv);
Kuser::UserServiceRpc_Stub stub(new KrpcChannel(), google::protobuf::Service::STUB_OWNS_CHANNEL);
Kuser::LoginRequest req;
Kuser::LoginResponse resp;
Krpccontroller controller;
stub.Login(&controller, &req, &resp, nullptr); // done 为空：阻塞到完成或超时
```

异步：传入 `Closure*`，`CallMethod` 立即返回。失败（借连接失败、熔断、超时）也一定会调 `done->Run()`。回调在 EventLoop 线程执行，不要做重计算。

```cpp
KrpcConnectPool::GetInstance().WarmUp(ip, port, KrpcApplication::CpuCores());
```

测完调用 `Shutdown()` / `Stop()`。

## 服务端

`NotifyService` 后 `Run()`。未实现的方法（例如示例里的 `Register`）会通过非空 `controller` 回错误帧，而不是空指针崩溃。  
`Ctrl+C` 或 `SIGTERM` 会退出事件循环、关掉 ZK 客户端（临时节点消失）。

对象池仍要求：借出的 `request`/`response`/`Closure` 在**同一条业务线程**归还。如果业务自己把 `done` 丢到别的线程再 `Run()`，不要用这个池。

## 编译与运行

依赖：Linux、CMake 3.16+、支持 C++20 的 GCC/Clang、Protobuf、Muduo
2.0.2、ZooKeeper C 客户端和 glog。

Ubuntu 20.04/22.04 先安装系统依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git \
  libboost-dev libboost-test-dev \
  libgoogle-glog-dev libprotobuf-dev protobuf-compiler \
  libzookeeper-mt-dev
```

Ubuntu 官方仓库不提供 Muduo 开发包。项目脚本会把固定版本安装到用户目录，
不覆盖系统文件：

```bash
./scripts/install_muduo.sh "$HOME/.local"
```

构建并运行离线测试：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

需要检查内存安全时使用独立的 ASan 构建目录：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/.local" \
  -DKRPC_ENABLE_ASAN=ON
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

功能示例需要先启动 ZooKeeper，再分别启动服务端和客户端：

```bash
# 先启动 ZooKeeper，再启动 server，再启动 client
./build/bin/server -i bin/test.conf
./build/bin/client -i bin/test.conf
```

改 `example/user.proto` 后：

```bash
cd example
protoc --cpp_out=. user.proto
```

改 `src/Krpcheader.proto` 后：

```bash
cd src
protoc --cpp_out=. Krpcheader.proto
mv Krpcheader.pb.h include/
```

## 模块

| 文件 | 作用 |
|------|------|
| `RpcCodec` | 统一编解码、长度校验、防溢出 |
| `LockFreeQueue` | 连接可发队列（MPMC） |
| `KrpcConnectPool` | 连接池；限制同时建连数；按 inflight 再出借 |
| `Krpcchannel` | 同步 `wait_for`，异步 TimeoutWheel；失败必调 done |
| `ConsistentHash` / `ServiceDiscovery` | COW 读 + 写锁；ZK 重连后刷新 |
| `CircuitBreaker` | 按节点；Half-Open 只放行 1 个探测 |
| `Krpcprovider` | 拆包、过载拒绝、错误回包、优雅退出 |
| `zookeeperutil` | 每客户端独立会话；过期后重建并重放 Create/Watch |
| `ZeroCopySend` | 阈值以上 `MSG_ZEROCOPY`（可选路径，不是正确性前提） |

## 已知边界

- 零拷贝仍依赖建连时扫 `/proc/self/fd` 找套接字，虚机上内核可能 `copied=1`
- 熔断仍是连续失败次数，不是时间窗失败率
- 异步调用失败不会自动换节点（只有同步会试一次）
- 没有 ASan 流水线；本地可用 `-fsanitize=address` 自行编一版
