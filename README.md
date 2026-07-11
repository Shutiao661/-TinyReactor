<!-- omit in toc -->
# TinyReactor

高性能多线程 TCP Echo 服务器 —— 基于 **Reactor 模式 + one loop per thread** 的 Windows C++20 网络库（学习项目）。

<!-- omit in toc -->
## 目录

- [架构设计](#架构设计)
- [快速开始](#快速开始)
- [命令行参数](#命令行参数)
- [项目结构](#项目结构)
- [核心特性](#核心特性)
- [技术栈](#技术栈)
- [已知局限](#已知局限)
- [路线图](#路线图)
- [许可](#许可)

## 架构设计

```
┌─────────────────────────────────────────────────┐
│                    main thread                    │
│  ┌───────────┐                                  │
│  │ Acceptor  │── accept() ──→ 轮询分发新连接      │
│  │ baseLoop  │                                  │
│  └───────────┘                                  │
└──────────────┬───────┬───────┬──────────────────┘
               │       │       │
        ┌──────▼─┐ ┌───▼───┐ ┌▼──────┐
        │Worker 0│ │Worker1│ │Worker2│  × N
        │EventLoop│ │EventLoop│ │EventLoop│
        │  select()  │  select()  │  select()
        └──────┬─┘ └───┬───┘ └──┬─────┘
               │       │       │
        ┌──────▼───────▼───────▼──────┐
        │      TcpConnection × N       │
        │  非阻塞 I/O + 对象池复用       │
        └──────────────────────────────┘
```

- **1 个 baseLoop** 负责 `accept()`，接受新连接后轮询分发给 worker
- **N 个 workerLoop** 负责客户端 I/O（读写事件 + 业务回调）
- **每个 EventLoop 绑定一个线程**，无锁设计（只在跨线程投递任务时加锁）

## 快速开始

### 环境要求

- Windows 10+
- Visual Studio 2022+（MSVC v143+）
- CMake 3.16+

### 构建

```bash
git clone https://github.com/Shutiao661/-TinyReactor.git
cd TinyReactor
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### 运行

```bash
# 默认：8080 端口，4 个 worker，最大 10000 连接
.\Release\TinyReactor.exe

# 自定义
.\Release\TinyReactor.exe 9000 8 50000
```

### 测试

```bash
# 启动服务器后，另开终端
telnet 127.0.0.1 8080
> hello
Echo: hello
> world
Echo: world
```

按 `Ctrl+C` 优雅退出。

### 验证结果

**2026-07-11 端到端测试通过**（MSVC v145, Windows 10, C++20）：

```
[1] hello    -> Echo: hello      ✓ 英文回显
[2] 你好     -> Echo: 你好       ✓ 中文回显（UTF-8）
[3] world    -> Echo: world      ✓ 多次连接复用
```

| 构建类型 | 状态 |
|----------|------|
| Debug    | ✅ 编译通过 + 回显正确 |
| Release  | ✅ 编译通过 + 回显正确 |

## 命令行参数

```
TinyReactor.exe [port] [workers] [maxConnections]

  port           监听端口（默认 8080）
  workers        worker 线程数（默认 4）
  maxConnections 最大并发连接数（默认 10000）
```

## 项目结构

```
TinyReactor/
├── main.cpp            # 入口：配置解析、线程管理、优雅退出
├── EventLoop.h/cpp     # 事件循环：select() 多路复用 + 读写事件 + 定时器
├── TcpConnection.h/cpp # TCP 连接：非阻塞 I/O、半写保护、缓冲区上限
├── Acceptor.h/cpp      # TCP 监听器
├── ObjectPool.h        # 泛型对象池（placement new）
├── AsyncLogger.h/cpp   # 异步日志（双缓冲）
├── CMakeLists.txt      # CMake 构建
└── README.md
```

## 核心特性

### I/O 模型

| 特性 | 实现 |
|---|---|
| 多路复用 | `select()`（学习用途，生产环境应换 IOCP） |
| 读写分离 | `readfds` + `writefds` 独立管理 |
| 半写保护 | `sendPending()` 循环发送，`WSAEWOULDBLOCK` 时注册写事件，发完后取消 |
| 监听/连接分离 | 监听 socket 跳过 `recv()`，直接触 `accept()` |

### 内存管理

| 特性 | 实现 |
|---|---|
| 对象池 | placement new 复用 TcpConnection，避免频繁 new/delete |
| 双缓冲日志 | `currentBuffer_` ↔ `nextBuffer_` swap，减少锁竞争 |
| 缓冲区上限 | 读缓冲 64KB / 写缓冲 256KB，防 DoS |

### 线程安全

| 场景 | 策略 |
|---|---|
| 跨线程任务投递 | `queueInLoop()` + `wakeup()` 唤醒 `select()` |
| 临界区最小化 | swap 到栈上再执行回调，不在锁内执行业务代码 |
| 连接关闭 | `closed_` 原子标志，`queueInLoop` 延迟移除防迭代器失效 |

### 优雅退出

```
Phase 1/4 → 停止 Acceptor（不再 accept）
Phase 2/4 → 停止 baseLoop
Phase 3/4 → 停止所有 workerLoop
Phase 4/4 → 停止 AsyncLogger（刷盘）
```

## 技术栈

- **语言**: C++20
- **编译器**: MSVC v145（Visual Studio 2026）
- **平台**: Windows / WinSock2
- **构建**: CMake 3.16+
- **编码**: UTF-8（`/utf-8` 编译选项）

## 已知局限

> 这是一个**学习项目**，用于理解 Reactor 模式的本质。以下局限是刻意保留的（面试时可展开讨论其改进方案）：

| 局限 | 说明 | 改进方向 |
|---|---|---|
| `select()` 而非 IOCP | Windows 上 IOCP 才是生产级方案 | Proactor 模式重写 |
| 无心跳/空闲检测 | 死连接不会自动清理 | 定时器 + 空闲超时 |
| 无 TLS/SSL | 明文传输 | OpenSSL / SChannel |
| 无上层应用协议 | 仅 `\n` 分帧 | HTTP / 自定义协议 |
| 无负载均衡 | 简单轮询，不考虑连接负载 | 最少连接数 / 一致性哈希 |
| 无单元测试 | 手动验证 | Google Test |

## 路线图

- [x] Reactor 模式 + one loop per thread
- [x] send() 半写保护 + 写事件监听
- [x] 对象池复用 TcpConnection
- [x] 双缓冲异步日志
- [x] 缓冲区上限防 DoS
- [x] 优雅退出
- [x] 连接数限制
- [x] 全链路错误处理
- [ ] IOCP / Proactor 版本
- [ ] HTTP 协议支持
- [ ] 定时器完善（连接超时、心跳）
- [ ] 基准测试（wrk / ab 压测数据）

## 许可

MIT License
