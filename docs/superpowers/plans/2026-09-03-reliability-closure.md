# Krpc Reliability Closure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Krpc 核心可靠性缺口，并提供可重复运行的单元测试、ZooKeeper 集成测试、演示、压测和 CI 证据。

**Architecture:** 保留现有 Muduo、Protobuf、ZooKeeper 与连接池结构，只在生命周期、尝试协调和任务计数处增加小型边界对象。外部环境由 shell 测试夹具统一管理，CI 将快速单测和 Docker 集成测试分开运行。

**Tech Stack:** C++20、Muduo 2.0.2、Protobuf、ZooKeeper C API、CMake/CTest、Bash、Docker Compose、GitHub Actions。

## Global Constraints

- 已提交给 TCP 连接的请求绝不自动重放。
- 同步和异步最终结果都只能完成一次；异步 `done->Run()` 只能调用一次。
- 不新增 TLS、鉴权、OpenTelemetry、业务 Fallback 或滑动窗口熔断。
- 不引入新的第三方 C++ 依赖。
- 所有代码修复必须先添加能稳定失败的回归测试。
- 脚本退出时必须清理它启动的进程和容器。

---

## 文件结构

- `src/include/ZkHandleGuard.h`、`src/ZkHandleGuard.cc`：串行化 ZooKeeper 句柄调用与替换。
- `src/zookeeperutil.cc`、`src/include/zookeeperutil.h`：通过句柄保护对象访问 ZooKeeper。
- `src/include/RetryAttemptState.h`：封装发送前失败是否允许第二次尝试和单次完成规则。
- `src/Krpcchannel.cc`：同步/异步共用尝试状态并调度备选节点。
- `src/include/PendingWork.h`：原子预留过载名额，并合并业务/发送阶段计数。
- `src/Krpcprovider.cc`、`src/include/Krpcprovider.h`：使用工作计数完成过载和退出。
- `tests/krpc_test.cc`：并发、重试、过载和退出状态单测。
- `tests/integration_smoke.sh`：ZooKeeper、双实例、Login 和 EchoBlob 集成验收。
- `scripts/runtime_common.sh`：Compose 检测、探活、进程和容器清理。
- `scripts/demo.sh`、`scripts/bench.sh`：使用公共运行时辅助并记录完整环境。
- `.github/workflows/ci.yml`：增加独立集成测试 Job。
- `README.md`、`docs/benchmark-results.md`、`OPTIMIZATION_PLAN.md`、`/home/lhs/Desktop/Krpc未完成清单.md`：同步真实状态和证据。

### Task 1: ZooKeeper 句柄生命周期

**Files:**
- Create: `src/include/ZkHandleGuard.h`
- Create: `src/ZkHandleGuard.cc`
- Modify: `src/zookeeperutil.cc`
- Modify: `src/include/zookeeperutil.h`
- Modify: `src/CMakeLists.txt`
- Test: `tests/krpc_test.cc`

**Interfaces:**
- Produces: `ZkHandleGuard::WithHandle(Fn)`，在句柄有效时执行 ZooKeeper 调用。
- Produces: `ZkHandleGuard::Replace(zhandle_t*)`，与所有调用互斥地替换旧句柄。
- Produces: `ZkHandleGuard::Close()`，与所有调用互斥地关闭句柄。

- [ ] **Step 1: 写并发句柄保护失败测试**

在 `tests/krpc_test.cc` 使用一个测试句柄和两个线程：第一个线程进入 `WithHandle` 后阻塞，第二个线程调用 `Replace`。断言第一个调用退出前 `Replace` 不能完成。

- [ ] **Step 2: 运行单测并确认编译因接口不存在而失败**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: FAIL，提示 `ZkHandleGuard` 不存在。

- [ ] **Step 3: 实现最小句柄保护并替换所有直接句柄调用**

保护类使用单独的 `std::mutex`；`WithHandle` 持锁检查句柄并执行函数；`Replace` 持锁关闭旧句柄后保存新句柄；`Close` 调用 `Replace(nullptr)`。`ZkClient` 的状态锁 `m_mu` 不用于保护 ZooKeeper C API。

- [ ] **Step 4: 运行单测和 ASan**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: PASS，`krpc_tests ok`。

- [ ] **Step 5: 提交**

```bash
git add src tests
git commit -m "fix: serialize ZooKeeper handle lifecycle"
```

### Task 2: 发送前失败换节点

**Files:**
- Create: `src/include/RetryAttemptState.h`
- Modify: `src/Krpcchannel.cc`
- Test: `tests/krpc_test.cc`

**Interfaces:**
- Produces: `RetryAttemptState::TryStartRetry(int error_code, bool request_submitted)`。
- Produces: `RetryAttemptState::TryFinish()`，保证最终完成只有一个赢家。

- [ ] **Step 1: 写失败矩阵测试**

覆盖借连接失败、EventLoop 发送前关闭、已提交后断连、超时、过载和两个并发完成者。只有前两种 `kRpcConnectFail && !request_submitted` 可以启动一次重试。

- [ ] **Step 2: 运行测试确认新场景失败**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: FAIL，发送前 EventLoop 关闭场景未获得重试资格。

- [ ] **Step 3: 重构一次尝试的完成回调**

`IssueOnce` 接收尝试完成回调；连接真正调用 `send` 前保持 `request_submitted=false`，提交 pending 并执行 send 后设为 `true`。发送前失败回到协调层选择 `exclude=node` 的备选节点。最终成功或不可重试错误通过 `TryFinish()` 更新 controller、指标并调用一次 done。

- [ ] **Step 4: 运行全量单测**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git add src/Krpcchannel.cc src/include/RetryAttemptState.h tests/krpc_test.cc
git commit -m "fix: retry all safe pre-send connection failures"
```

### Task 3: 原子过载名额与响应 drain

**Files:**
- Create: `src/include/PendingWork.h`
- Modify: `src/include/Krpcprovider.h`
- Modify: `src/Krpcprovider.cc`
- Test: `tests/krpc_test.cc`

**Interfaces:**
- Produces: `PendingWork::TryAcquireJob(int limit)`、`ReleaseJob()`。
- Produces: `PendingWork::BeginSend()`、`EndSend()`、`Total()`。

- [ ] **Step 1: 写并发上限和发送阶段测试**

32 个线程同时争抢上限为 4 的名额，断言成功数和峰值不超过 4；业务名额释放但发送计数仍为 1 时，退出状态不得视为空闲。

- [ ] **Step 2: 运行测试确认接口缺失**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: FAIL，提示 `PendingWork` 不存在。

- [ ] **Step 3: 用 compare-exchange 实现名额预留**

`TryAcquireJob` 在当前值小于上限时 CAS 加一；所有返回路径通过 RAII 或唯一收尾点释放。`SendFrame` 调度前 `BeginSend`，EventLoop 中发送或放弃后 `EndSend`。退出判断使用 `Total()`。

- [ ] **Step 4: 运行单测**

Run: `cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git add src tests
git commit -m "fix: enforce overload limit and drain queued responses"
```

### Task 4: 可复现运行环境与集成测试

**Files:**
- Create: `scripts/runtime_common.sh`
- Create: `tests/script_test.sh`
- Create: `tests/integration_smoke.sh`
- Modify: `scripts/demo.sh`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `compose_cmd` 数组，优先 Compose V2，兼容 `docker-compose`。
- Produces: `wait_for_zookeeper`、`wait_for_process`、`cleanup_runtime`。

- [ ] **Step 1: 写 shell 失败验收**

使用替身命令验证缺少 Compose 时返回非零、ZooKeeper 30 秒未就绪时返回非零、cleanup 会停止两个 server 并执行 compose down。

- [ ] **Step 2: 运行测试确认现有 demo 失败行为不符合要求**

Run: `bash tests/script_test.sh`

Expected: FAIL，现有脚本没有 fallback、超时失败和容器清理。

- [ ] **Step 3: 实现公共辅助并改造 demo**

不再依赖 `nc`：使用容器内 `zkServer.sh status` 或 Bash `/dev/tcp` 做端口探活，再用客户端实际 RPC 作为最终就绪证据。trap 只清理本次启动的资源。

- [ ] **Step 4: 加入双实例断言**

集成脚本启动两个服务后，使用 ZooKeeper CLI 查询服务路径，断言同时存在 `127.0.0.1:8000` 和 `127.0.0.1:8001`，随后运行 client。

- [ ] **Step 5: 运行集成测试**

Run: `KRPC_RUN_INTEGRATION=1 ctest --test-dir build -R krpc_integration --output-on-failure`

Expected: PASS；退出后没有本次启动的 server 或 ZooKeeper 容器。

- [ ] **Step 6: 提交**

```bash
git add scripts tests
git commit -m "test: add reproducible ZooKeeper integration smoke"
```

### Task 5: 压测元数据、CI 和文档

**Files:**
- Modify: `scripts/bench.sh`
- Modify: `.github/workflows/ci.yml`
- Modify: `README.md`
- Modify: `docs/benchmark-results.md`
- Modify: `OPTIMIZATION_PLAN.md`
- Modify: `/home/lhs/Desktop/Krpc未完成清单.md`

**Interfaces:**
- Produces: 压测输出中的 `date`、`hostname`、`cpu`、`ram`、`kernel`、`compiler`、`build_type`、`command`。

- [ ] **Step 1: 添加脚本输出断言**

运行 `bench.sh` 的元数据模式，检查八个字段全部非空。

- [ ] **Step 2: 改造 bench 并运行断言**

Run: `KRPC_BENCH_METADATA_ONLY=1 ./scripts/bench.sh`

Expected: PASS，八个字段均输出。

- [ ] **Step 3: CI 增加 integration Job**

该 Job 只运行一次 Release 构建，执行 `KRPC_RUN_INTEGRATION=1 ctest -R krpc_integration`，并在 `always()` 步骤清理 Compose。

- [ ] **Step 4: 更新文档和未完成清单**

README 写清依赖和执行边界；删除 `OPTIMIZATION_PLAN.md` 重复标题；清单将已修项移动到“本地已验证”，保留尚未完成的故障演示和真实压测数据。

- [ ] **Step 5: 全量验证**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$HOME/.local" -DKRPC_ENABLE_ASAN=ON
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

Expected: 所有测试通过，ASan 无错误。

- [ ] **Step 6: 提交**

```bash
git add scripts .github README.md docs OPTIMIZATION_PLAN.md
git commit -m "ci: verify integration workflow and benchmark metadata"
```
