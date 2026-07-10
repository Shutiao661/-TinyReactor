// EventLoop.cpp —— 事件循环实现（读写事件 + 定时器 + 错误处理）
#include "EventLoop.h"
#include "TcpConnection.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#pragma comment(lib, "ws2_32.lib")

// ========== 构造 / 析构 ==========

EventLoop::EventLoop() {
    // 用 TCP socket pair 模拟 Unix pipe 做唤醒
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        std::cerr << "[EventLoop] socket() failed: " << WSAGetLastError() << "\n";
        abort();
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 系统自动分配端口

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[EventLoop] bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        abort();
    }

    if (listen(listener, 1) == SOCKET_ERROR) {
        std::cerr << "[EventLoop] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        abort();
    }

    int len = sizeof(addr);
    if (getsockname(listener, (sockaddr*)&addr, &len) == SOCKET_ERROR) {
        std::cerr << "[EventLoop] getsockname() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        abort();
    }

    wakeupWrite_ = socket(AF_INET, SOCK_STREAM, 0);
    if (wakeupWrite_ == INVALID_SOCKET) {
        std::cerr << "[EventLoop] wakeupWrite socket() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        abort();
    }

    if (connect(wakeupWrite_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[EventLoop] connect() failed: " << WSAGetLastError() << "\n";
        closesocket(wakeupWrite_);
        closesocket(listener);
        abort();
    }

    wakeupRead_ = accept(listener, (sockaddr*)&addr, &len);
    if (wakeupRead_ == INVALID_SOCKET) {
        std::cerr << "[EventLoop] accept() failed: " << WSAGetLastError() << "\n";
        closesocket(wakeupWrite_);
        closesocket(listener);
        abort();
    }

    closesocket(listener);

    // 设置两个 socket 为非阻塞
    u_long mode = 1;
    ioctlsocket(wakeupRead_,  FIONBIO, &mode);
    ioctlsocket(wakeupWrite_, FIONBIO, &mode);
}

EventLoop::~EventLoop() {
    quit();
    if (wakeupRead_  != INVALID_SOCKET) closesocket(wakeupRead_);
    if (wakeupWrite_ != INVALID_SOCKET) closesocket(wakeupWrite_);
}

// ========== 主循环 ==========

void EventLoop::loop() {
    // 在首次调用 loop() 时绑定线程 ID（比在构造函数中更可靠）
    bool expected = false;
    if (!threadIdSet_.compare_exchange_strong(expected, true)) {
        std::cerr << "[EventLoop] loop() called more than once or from multiple threads — aborting\n";
        abort();
    }
    threadId_ = std::this_thread::get_id();

    looping_ = true;

    while (!quit_.load(std::memory_order_acquire)) {
        fd_set readfds, writefds;
        int maxfd = 0;
        buildFdSets(readfds, writefds, maxfd);

        timeval timeout{1, 0};  // 1 秒超时，同时用于定时器检查
        int ret = select(maxfd + 1, &readfds, &writefds, nullptr, &timeout);

        if (ret < 0) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;  // 被信号中断，继续
            std::cerr << "[EventLoop] select() error: " << err << "\n";
            break;
        }

        // 1. 处理唤醒
        if (FD_ISSET(wakeupRead_, &readfds)) {
            handleWakeup();
        }

        // 2. 处理读事件（拷贝迭代，因为回调中可能修改 connections_）
        {
            auto conns = connections_;  // 浅拷贝指针数组
            for (auto* conn : conns) {
                if (conn->fd() != INVALID_SOCKET && FD_ISSET(conn->fd(), &readfds)) {
                    conn->handleRead();
                }
            }
        }

        // 3. 处理写事件（拷贝迭代）
        {
            auto wconns = std::vector<TcpConnection*>(writeConnections_.begin(), writeConnections_.end());
            for (auto* conn : wconns) {
                if (conn->fd() != INVALID_SOCKET && FD_ISSET(conn->fd(), &writefds)) {
                    conn->handleWrite();
                }
            }
        }

        // 4. 执行投递任务
        doPendingFunctors();

        // 5. 处理到期定时器
        processTimers();
    }

    looping_ = false;
}

void EventLoop::buildFdSets(fd_set& readfds, fd_set& writefds, int& maxfd) {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);

    FD_SET(wakeupRead_, &readfds);
    maxfd = (int)wakeupRead_;

    for (auto* conn : connections_) {
        SOCKET fd = conn->fd();
        if (fd == INVALID_SOCKET) continue;
        FD_SET(fd, &readfds);
        if ((int)fd > maxfd) maxfd = (int)fd;
    }

    for (auto* conn : writeConnections_) {
        SOCKET fd = conn->fd();
        if (fd == INVALID_SOCKET) continue;
        FD_SET(fd, &writefds);
        if ((int)fd > maxfd) maxfd = (int)fd;
    }
}

// ========== 退出 ==========

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!isInLoopThread()) {
        wakeup();
    }
}

// ========== 任务投递 ==========

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    if (!isInLoopThread()) {
        wakeup();
    }
}

bool EventLoop::isInLoopThread() const {
    return std::this_thread::get_id() == threadId_;
}

// ========== 连接管理 ==========

void EventLoop::addConnection(TcpConnection* conn) {
    assert(isInLoopThread());
    connections_.push_back(conn);
}

void EventLoop::removeConnection(TcpConnection* conn) {
    assert(isInLoopThread());
    auto it = std::find(connections_.begin(), connections_.end(), conn);
    if (it != connections_.end()) {
        connections_.erase(it);
    }
}

// ========== 写事件管理 ==========

void EventLoop::enableWrite(TcpConnection* conn) {
    assert(isInLoopThread());
    writeConnections_.insert(conn);
}

void EventLoop::disableWrite(TcpConnection* conn) {
    assert(isInLoopThread());
    writeConnections_.erase(conn);
}

// ========== 唤醒机制 ==========

void EventLoop::wakeup() {
    char c = 'x';
    ::send(wakeupWrite_, &c, 1, 0);  // 非阻塞，忽略错误
}

void EventLoop::handleWakeup() {
    char buf[256];
    while (recv(wakeupRead_, buf, sizeof(buf), 0) > 0) {}
}

// ========== 待执行任务 ==========

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (auto& f : functors) {
        if (f) f();
    }
}

// ========== 定时器 ==========

void EventLoop::runAfter(int delayMs, Functor cb) {
    assert(isInLoopThread());
    int64_t now = []() -> int64_t {
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        return (counter.QuadPart * 1000) / freq.QuadPart;
    }();
    timers_.insert({now + delayMs, nextTimerId_++, std::move(cb)});
}

void EventLoop::processTimers() {
    int64_t now = []() -> int64_t {
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        return (counter.QuadPart * 1000) / freq.QuadPart;
    }();

    expiredTimers_.clear();
    // 收集所有到期定时器（不直接在遍历中执行回调，避免 re-entrancy 问题）
    for (auto it = timers_.begin(); it != timers_.end(); ) {
        if (it->deadlineMs <= now) {
            expiredTimers_.push_back(std::move(it->cb));
            it = timers_.erase(it);
        } else {
            break;  // 有序 set，后面的都没到期
        }
    }

    for (auto& cb : expiredTimers_) {
        if (cb) cb();
    }
}
