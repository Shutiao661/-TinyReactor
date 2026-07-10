// TcpConnection.cpp —— TCP 连接实现（半写保护、缓冲区上限、安全关闭）
#include "TcpConnection.h"
#include "EventLoop.h"
#include "ObjectPool.h"

#include <cassert>
#include <iostream>

// 全局连接池（main.cpp 中定义）
extern ObjectPool<TcpConnection> connPool;

// ========== 构造 / 析构 ==========

TcpConnection::TcpConnection(EventLoop* loop, SOCKET fd)
    : loop_(loop), fd_(fd) {
    assert(loop_);
    assert(fd_ != INVALID_SOCKET);

    u_long mode = 1;
    if (ioctlsocket(fd_, FIONBIO, &mode) == SOCKET_ERROR) {
        std::cerr << "[TcpConnection] ioctlsocket(FIONBIO) failed: " << WSAGetLastError() << "\n";
    }
}

TcpConnection::~TcpConnection() {
    if (fd_ != INVALID_SOCKET) {
        closesocket(fd_);
        fd_ = INVALID_SOCKET;
    }
}

// ========== 读事件 ==========

void TcpConnection::handleRead() {
    if (closed_.load(std::memory_order_acquire)) return;

    // 监听 socket：select 可读 = 有新连接，直接回调 acceptor，不做 recv
    if (isListening_) {
        if (readCallback_) readCallback_(this, "");
        return;
    }

    char buf[65536];
    int n = recv(fd_, buf, sizeof(buf), 0);

    if (n > 0) {
        // 缓冲区上限保护
        if (readBuffer_.size() + n > kMaxReadBufferSize) {
            std::cerr << "[TcpConnection] read buffer overflow, closing fd=" << fd_ << "\n";
            handleClose();
            return;
        }

        readBuffer_.append(buf, n);

        // 按 '\n' 拆包
        size_t pos;
        while ((pos = readBuffer_.find('\n')) != std::string::npos) {
            std::string msg = readBuffer_.substr(0, pos);
            readBuffer_.erase(0, pos + 1);
            if (readCallback_) {
                readCallback_(this, msg);
            }
            // 回调中可能调用了 shutdown()，需检查
            if (closed_.load(std::memory_order_acquire)) return;
        }
    } else if (n == 0) {
        // 对端正常关闭
        handleClose();
    } else {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            std::cerr << "[TcpConnection] recv() error " << err << " on fd=" << fd_ << "\n";
            handleClose();
        }
        // WSAEWOULDBLOCK: 非阻塞读没数据，忽略
    }
}

// ========== 写事件 ==========

void TcpConnection::handleWrite() {
    if (closed_.load(std::memory_order_acquire)) return;
    sendPending();
}

// ========== 发送（线程安全入口）==========

void TcpConnection::send(std::string msg) {
    if (closed_.load(std::memory_order_acquire)) return;

    if (loop_->isInLoopThread()) {
        sendInLoop(std::move(msg));
    } else {
        // 跨线程：拷贝 msg 投递到 loop 线程
        loop_->queueInLoop([this, msg = std::move(msg)]() mutable {
            sendInLoop(std::move(msg));
        });
    }
}

// ========== 发送（loop 线程内）==========

void TcpConnection::sendInLoop(std::string msg) {
    if (closed_.load(std::memory_order_acquire)) return;

    // 写缓冲区上限保护
    if (writeBuffer_.size() + msg.size() + 1 > kMaxWriteBufferSize) {
        std::cerr << "[TcpConnection] write buffer overflow, closing fd=" << fd_ << "\n";
        handleClose();
        return;
    }

    writeBuffer_ += msg;
    writeBuffer_ += '\n';

    if (!isSending_) {
        isSending_ = true;
        sendPending();
    }
    // 如果已经在发送中，数据已追加到 writeBuffer_，等到 handleWrite 回调时继续发送
}

void TcpConnection::sendPending() {
    if (closed_.load(std::memory_order_acquire)) return;

    while (!writeBuffer_.empty()) {
        int n = ::send(fd_, writeBuffer_.c_str(), (int)writeBuffer_.size(), 0);

        if (n > 0) {
            writeBuffer_.erase(0, n);
        } else if (n == 0) {
            // 连接关闭
            handleClose();
            return;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                // 发送缓冲区满，注册可写事件，等待回调
                loop_->enableWrite(this);
                return;
            }
            // 其他错误
            std::cerr << "[TcpConnection] send() error " << err << " on fd=" << fd_ << "\n";
            handleClose();
            return;
        }
    }

    // 全部发送完毕，取消写事件监听
    isSending_ = false;
    loop_->disableWrite(this);
}

// ========== 连接关闭 ==========

void TcpConnection::shutdown() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;  // 已关闭

    if (loop_->isInLoopThread()) {
        handleClose();
    } else {
        loop_->queueInLoop([this] { handleClose(); });
    }
}

void TcpConnection::handleClose() {
    // 防止重复关闭
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;

    isSending_ = false;

    if (fd_ != INVALID_SOCKET) {
        ::shutdown(fd_, SD_BOTH);
        closesocket(fd_);
        fd_ = INVALID_SOCKET;
    }

    if (closeCallback_) {
        closeCallback_(this);
    }

    // 监听连接由 Acceptor 管理，不归还对象池
    if (isListening_) return;

    // 延迟到 doPendingFunctors 中移除，避免在 EventLoop 迭代 connections_ 时修改
    loop_->queueInLoop([this] {
        loop_->disableWrite(this);
        loop_->removeConnection(this);
        connPool.release(this);
    });
}
