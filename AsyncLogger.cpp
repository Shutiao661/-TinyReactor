// AsyncLogger.cpp —— 异步日志实现（双缓冲 + 条件变量）
#include "AsyncLogger.h"

#include <iostream>
#include <chrono>
#include <Windows.h>

// ========== 单例 ==========

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger logger;
    return logger;
}

// ========== 构造 / 析构 ==========

AsyncLogger::AsyncLogger() {
    currentBuffer_ = std::make_unique<Buffer>(kBufferSize);
    nextBuffer_    = std::make_unique<Buffer>(kBufferSize);
}

AsyncLogger::~AsyncLogger() {
    stop();
}

// ========== 前端 ==========

void AsyncLogger::append(const char* data, size_t len) {
    if (!running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (currentBuffer_->size() + len > currentBuffer_->capacity()) {
        buffersToWrite_.push_back(std::move(currentBuffer_));

        if (nextBuffer_) {
            currentBuffer_ = std::move(nextBuffer_);
        } else {
            currentBuffer_ = std::make_unique<Buffer>(kBufferSize);
        }
    }

    currentBuffer_->insert(currentBuffer_->end(), data, data + len);
    cond_.notify_one();
}

// ========== 启动 ==========

void AsyncLogger::start(const std::string& filePath) {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;  // 已启动

    logFile_.open(filePath, std::ios::app);
    if (!logFile_.is_open()) {
        std::cerr << "[AsyncLogger] failed to open log file: " << filePath << "\n";
    }

    backendThread_ = std::thread(&AsyncLogger::threadFunc, this);
    std::cout << "[AsyncLogger] started, writing to " << filePath << "\n";
}

// ========== 停止 ==========

void AsyncLogger::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;  // 已停止

    cond_.notify_all();
    if (backendThread_.joinable()) {
        backendThread_.join();
    }

    // 刷盘残余数据
    if (currentBuffer_ && currentBuffer_->size() > 0) {
        logFile_.write(currentBuffer_->data(), currentBuffer_->size());
    }
    for (auto& buf : buffersToWrite_) {
        if (buf && buf->size() > 0) {
            logFile_.write(buf->data(), buf->size());
        }
    }
    logFile_.flush();
    logFile_.close();

    std::cout << "[AsyncLogger] stopped, all data flushed\n";
}

// ========== 后端线程 ==========

void AsyncLogger::threadFunc() {
    std::vector<std::unique_ptr<Buffer>> buffersToWrite;

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait_for(lock, std::chrono::seconds(kFlushIntervalSec), [this] {
                return !buffersToWrite_.empty() || !running_.load(std::memory_order_acquire);
            });

            buffersToWrite_.swap(buffersToWrite);

            if (!nextBuffer_) {
                nextBuffer_ = std::make_unique<Buffer>(kBufferSize);
            }
        }

        // 无锁写入文件
        for (auto& buf : buffersToWrite) {
            if (buf && buf->size() > 0) {
                logFile_.write(buf->data(), buf->size());
            }
        }

        if (buffersToWrite.size() > 2) {
            buffersToWrite.resize(2);  // 保留两个缓冲复用，其余释放
        }

        logFile_.flush();
        buffersToWrite.clear();
    }
}
