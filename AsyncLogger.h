// AsyncLogger.h —— 异步日志（双缓冲 + 后台线程）
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>

class AsyncLogger {
public:
    static AsyncLogger& instance();

    void append(const char* data, size_t len);
    void start(const std::string& filePath = "server.log");
    void stop();

private:
    AsyncLogger();
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void threadFunc();

    using Buffer = std::vector<char>;

    std::unique_ptr<Buffer> currentBuffer_;
    std::unique_ptr<Buffer> nextBuffer_;
    std::vector<std::unique_ptr<Buffer>> buffersToWrite_;

    std::mutex mutex_;
    std::condition_variable cond_;
    std::thread backendThread_;
    std::atomic<bool> running_{false};
    std::ofstream logFile_;

    static constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4MB
    static constexpr int    kFlushIntervalSec = 3;
};
