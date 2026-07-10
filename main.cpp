// main.cpp —— TinyReactor：高性能多线程 TCP Echo Server
//
// 架构：Reactor 模式 + one loop per thread
//   baseThread (1): Acceptor 监听 + 接受新连接
//   workerThread (N): TcpConnection I/O + 业务处理
//
// 优化要点：
//   - send() 半写保护 + 写事件监听
//   - 读写缓冲区上限（防 DoS）
//   - 最大连接数限制
//   - 优雅退出（graceful shutdown）
//   - 全链路错误处理
//   - 对象池复用 TcpConnection
//   - 异步日志（双缓冲）

#include <WinSock2.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <csignal>

#include "EventLoop.h"
#include "TcpConnection.h"
#include "Acceptor.h"
#include "ObjectPool.h"
#include "AsyncLogger.h"

#pragma comment(lib, "ws2_32.lib")

// ==================== 全局配置 ====================
struct Config {
    int         port            = 8080;
    int         workerCount     = 4;
    int         maxConnections  = 10000;
    std::string logFilePath     = "server.log";
} g_config;

// ==================== 全局状态 ====================
ObjectPool<TcpConnection> connPool;
std::atomic<int> g_activeConnections{0};
std::atomic<bool> g_shuttingDown{false};

// ==================== 业务回调 ====================
void onMessage(TcpConnection* conn, const std::string& msg) {
    conn->send("Echo: " + msg);

    // 纳秒时间戳日志
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    auto ns = (counter.QuadPart * 1000000000LL) / freq.QuadPart;

    std::ostringstream oss;
    oss << "[" << ns << " ns] fd=" << conn->fd() << " msg=" << msg << "\n";
    std::string log = oss.str();
    AsyncLogger::instance().append(log.c_str(), log.size());
}

// ==================== 连接关闭回调 ====================
void onConnectionClosed(TcpConnection* conn) {
    (void)conn;
    int remaining = --g_activeConnections;
    if (remaining % 1000 == 0 || remaining < 10) {
        std::cout << "[main] active connections: " << remaining << "\n";
    }
}

// ==================== 主函数 ====================
int main(int argc, char* argv[]) {
    // ---- 解析命令行参数 ----
    if (argc > 1) g_config.port = std::atoi(argv[1]);
    if (argc > 2) g_config.workerCount = (std::max)(1, std::atoi(argv[2]));
    if (argc > 3) g_config.maxConnections = (std::max)(1, std::atoi(argv[3]));

    std::cout << "TinyReactor — High-Performance TCP Echo Server\n"
              << "  port:           " << g_config.port << "\n"
              << "  workers:        " << g_config.workerCount << "\n"
              << "  max connections:" << g_config.maxConnections << "\n"
              << "  log file:       " << g_config.logFilePath << "\n\n";

    // ---- 初始化 Winsock ----
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[main] WSAStartup() failed: " << WSAGetLastError() << "\n";
        return 1;
    }

    // ---- 启动异步日志 ----
    AsyncLogger::instance().start(g_config.logFilePath);

    // ==================== 创建 base EventLoop ====================
    std::promise<EventLoop*> basePromise;
    auto baseFuture = basePromise.get_future();

    std::thread baseThread([&basePromise]() {
        EventLoop loop;
        basePromise.set_value(&loop);
        loop.loop();
    });

    EventLoop* baseLoop = baseFuture.get();
    if (!baseLoop) {
        std::cerr << "[main] failed to get baseLoop\n";
        return 1;
    }

    // ==================== 创建 worker EventLoops ====================
    std::vector<EventLoop*> workerLoops;
    std::vector<std::thread> workerThreads;
    workerLoops.reserve(g_config.workerCount);
    workerThreads.reserve(g_config.workerCount);

    for (int i = 0; i < g_config.workerCount; ++i) {
        std::promise<EventLoop*> prom;
        auto future = prom.get_future();

        workerThreads.emplace_back([promise = std::move(prom)]() mutable {
            EventLoop loop;
            promise.set_value(&loop);
            loop.loop();
        });

        workerLoops.push_back(future.get());
        std::cout << "[main] worker " << i << " started\n";
    }

    // ==================== Acceptor (baseLoop) ====================
    Acceptor acceptor(baseLoop, g_config.port);

    int nextWorker = 0;
    acceptor.setNewConnectionCallback([&](SOCKET clientFd) {
        // 关闭中，不再接受新连接
        if (g_shuttingDown.load(std::memory_order_acquire)) {
            closesocket(clientFd);
            return;
        }

        // 连接数限制
        int current = g_activeConnections.load(std::memory_order_acquire);
        if (current >= g_config.maxConnections) {
            std::cerr << "[main] connection limit reached (" << g_config.maxConnections
                      << "), rejecting fd=" << clientFd << "\n";
            closesocket(clientFd);
            return;
        }

        // 轮询选择 worker
        EventLoop* worker = workerLoops[nextWorker % workerLoops.size()];
        ++nextWorker;

        TcpConnection* conn = connPool.acquire(worker, clientFd);
        if (!conn) {
            std::cerr << "[main] connPool.acquire() returned null\n";
            closesocket(clientFd);
            return;
        }

        conn->setReadCallback(onMessage);
        conn->setCloseCallback(onConnectionClosed);
        ++g_activeConnections;

        worker->queueInLoop([worker, conn]() {
            worker->addConnection(conn);
        });
    });

    // ==================== 等待退出信号 ====================
    std::cout << "\n>>> TinyReactor running. Press Ctrl+C to stop. <<<\n\n";

    // 注册 Ctrl+C 处理（优雅退出）
    signal(SIGINT, [](int) {
        std::cout << "\n[main] SIGINT received, shutting down gracefully...\n";
        g_shuttingDown.store(true, std::memory_order_release);
    });

    signal(SIGTERM, [](int) {
        std::cout << "\n[main] SIGTERM received, shutting down gracefully...\n";
        g_shuttingDown.store(true, std::memory_order_release);
    });

    // 等待 Ctrl+C
    while (!g_shuttingDown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ==================== 优雅退出 ====================
    std::cout << "[main] Phase 1/4: stopping acceptor...\n";
    acceptor.stop();

    std::cout << "[main] Phase 2/4: stopping base event loop...\n";
    baseLoop->quit();
    baseThread.join();
    std::cout << "[main] base loop stopped\n";

    std::cout << "[main] Phase 3/4: stopping worker loops...\n";
    for (int i = 0; i < g_config.workerCount; ++i) {
        workerLoops[i]->quit();
    }
    for (int i = 0; i < g_config.workerCount; ++i) {
        workerThreads[i].join();
        std::cout << "[main] worker " << i << " stopped\n";
    }

    std::cout << "[main] Phase 4/4: stopping logger...\n";
    AsyncLogger::instance().stop();

    // ---- 清理 Winsock ----
    WSACleanup();

    std::cout << "[main] TinyReactor exited cleanly. Goodbye!\n";
    return 0;
}
