// EventLoop.h —— 事件循环（支持读写事件、定时器）
#pragma once

#include <WinSock2.h>
#include <functional>
#include <vector>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>

class TcpConnection;

class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 非拷贝、非移动（socket 资源不可共享）
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();                        // 启动事件循环（阻塞）
    void quit();                        // 通知退出（线程安全）

    void queueInLoop(Functor cb);       // 线程安全的任务投递
    bool isInLoopThread() const;        // 判断当前线程是否为 loop 线程

    // ---- 连接管理（仅限 loop 线程调用）----
    void addConnection(TcpConnection* conn);
    void removeConnection(TcpConnection* conn);

    // ---- 写事件管理（仅限 loop 线程调用）----
    void enableWrite(TcpConnection* conn);
    void disableWrite(TcpConnection* conn);

    // ---- 定时器（仅限 loop 线程调用）----
    // 在 delayMs 毫秒后执行 cb（一次性定时器）
    void runAfter(int delayMs, Functor cb);

    // 获取活跃连接数
    size_t connectionCount() const { return connections_.size(); }

private:
    void wakeup();                      // 唤醒 select
    void handleWakeup();                // 消费唤醒数据
    void doPendingFunctors();           // 执行投递任务
    void processTimers();               // 检查并触发到期定时器

    // select 辅助：构建 fd_set
    void buildFdSets(fd_set& readfds, fd_set& writefds, int& maxfd);

    std::vector<TcpConnection*> connections_;
    std::set<TcpConnection*> writeConnections_;  // 有待发数据的连接

    std::atomic<bool> quit_{false};
    bool looping_ = false;
    std::thread::id threadId_;
    std::atomic<bool> threadIdSet_{false};  // 保证 loop() 只被一个线程调用

    SOCKET wakeupRead_  = INVALID_SOCKET;
    SOCKET wakeupWrite_ = INVALID_SOCKET;

    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;

    // 定时器：{到期时间戳(ms), 回调}
    struct Timer {
        int64_t deadlineMs;
        int64_t id;
        Functor cb;
        bool operator<(const Timer& rhs) const {
            if (deadlineMs != rhs.deadlineMs) return deadlineMs < rhs.deadlineMs;
            return id < rhs.id;
        }
    };
    // 使用 multiset 允许相同截止时间，定时器 id 保证唯一排序
    std::multiset<Timer> timers_;
    int64_t nextTimerId_ = 0;
    std::vector<Functor> expiredTimers_; // 复用，避免在回调中操作 timers_ 的问题
};
