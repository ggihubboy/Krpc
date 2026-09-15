# CI Protobuf Build Implementation Plan

> **For agentic workers:** Execute this plan task-by-task. Use TDD for the regression checks.

**Goal:** GitHub Ubuntu 24.04 能用当前 `protoc` 生成并编译 Protobuf C++，不再编译仓库里的旧 `.pb.*` 文件。

**Architecture:** 在 `find_package(Protobuf)` 前打开 `protobuf_MODULE_COMPATIBLE`，把 `Protobuf_PROTOC_EXECUTABLE` 解析成真实存在的二进制（或 `protobuf::protoc` 导入目标）。用 CMake 官方 `protobuf_generate_cpp` 在构建目录生成源文件，业务目标只链接 proto 库。

**Tech Stack:** CMake 3.16+、FindProtobuf / protobuf CONFIG、GitHub Actions Ubuntu 24.04、ctest。

## Global Constraints

- 不修改 RPC 运行时行为
- 本机 Protobuf 3.12 与 CI Protobuf 3.21 都必须能构建
- 源码树不保留 `.pb.cc` / `.pb.h`

---

### Task 1: 回归检查

- [ ] 在 `tests/script_test.sh` 增加：源码树不得有 `user.pb.*` / `Krpcheader.pb.*`
- [ ] 增加：CMake 解析 `protoc` 时必须检查路径真实存在（禁止把 `-NOTFOUND` 当可执行文件）
- [ ] 运行 `tests/script_test.sh`，确认新检查能跑通或按预期失败

### Task 2: 官方生成流程

- [ ] `CMakeLists.txt`：`protobuf_MODULE_COMPATIBLE ON` 后再 `find_package(Protobuf)`
- [ ] 重写 `cmake/KrpcProtobuf.cmake`：解析真实 `protoc`，调用 `protobuf_generate_cpp`，找不到该命令时用同一条 `protoc --cpp_out` 回退
- [ ] 业务目标继续链接 `krpc_header_proto` / `krpc_example_proto`，并用 `OBJECT_DEPENDS` 避免并行编译抢头文件

### Task 3: CI 与说明

- [ ] CI 在构建后检查生成文件，失败时打印详细编译日志
- [ ] 更新 README 中的生成说明

### Task 4: 验证

- [ ] 清空构建目录后 `cmake` + `cmake --build` + `ctest`
