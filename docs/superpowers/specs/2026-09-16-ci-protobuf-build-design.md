# CI Protobuf 构建设计

## 目标

修复 GitHub Actions 在 Ubuntu 24.04 上的 Protobuf 编译失败，同时保留本机
Protobuf 3.12 的构建能力。Debug、Release、ASan 和集成构建必须共用同一套生成流程。

## 方案

使用 CMake `FindProtobuf` 提供的官方生成命令，把两个 `.proto` 文件的生成结果放在
构建目录，不提交或编译源码目录中的 `.pb.cc` / `.pb.h`。

- `krpc_header_proto` 只负责 `Krpcheader.proto` 的生成和编译。
- `krpc_example_proto` 只负责 `user.proto` 的生成和编译。
- 业务目标通过链接对应的 proto 库获得头文件目录和构建依赖。
- CI 输出 `protoc` 版本，并在配置后确认生成规则存在；实际生成由构建阶段完成。

## 错误处理

找不到 Protobuf 或 `protoc` 时由 CMake 配置阶段直接失败并给出明确错误。生成失败时由
构建系统报告具体的 `protoc` 命令和退出状态，不再用自制的配置阶段命令遮蔽错误。

## 验证

1. 增加脚本检查，禁止 CMake 再引用源码目录中的旧 `.pb.*` 文件。
2. 本地重新配置并完成 Release 构建。
3. 运行全部 `ctest`。
4. 检查 CI YAML，确保四个任务都走相同构建入口。

## 范围

只修复 Protobuf/CMake/CI 构建链，不修改 RPC 功能、协议或运行时行为。
