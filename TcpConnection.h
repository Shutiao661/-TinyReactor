// TcpConnection.h —— TCP 连接（非阻塞读写、半写保护、缓冲区上限）
#pragma once

#include <WinSock2.h>
#include <string>
#include <functional>
#include <atomic>

class EventLoop;

class TcpConnection {
public:
    using ReadCallback  = std::function<void(TcpConnection*, const std::string&)>;
    using CloseCallback = std::function<void(TcpConnection*)>;

    TcpConnection(EventLoop* loop, SOCKET fd);
    ~TcpConnection();

    // 标记为监听 socket（handleRead 时不 recv，直接触发回调）
    void setListening(bool v) { isListening_ = v; }
    bool isListening() const { return isListening_; }

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    SOCKET fd() const { return fd_; }

    void setReadCallback (const ReadCallback& cb)  { readCallback_  = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }

    // 由 EventLoop 调用
    void handleRead();
    void handleWrite();

    // 发送（线程安全），自动追加 '\n'
    void send(std::string msg);

    // 主动关闭
    void shutdown();

    size_t readBufferSize()  const { return readBuffer_.size(); }
    size_t writeBufferSize() const { return writeBuffer_.size(); }

private:
    void sendInLoop(std::string msg);
    void sendPending();
    void handleClose();

    EventLoop* loop_;
    SOCKET fd_ = INVALID_SOCKET;

    ReadCallback  readCallback_;
    CloseCallback closeCallback_;

    std::string readBuffer_;
    std::string writeBuffer_;

    bool isSending_ = false;
    std::atomic<bool> closed_{false};
    bool isListening_ = false;                 // 监听 socket 跳过 recv

    static constexpr size_t kMaxReadBufferSize  = 64 * 1024;
    static constexpr size_t kMaxWriteBufferSize = 256 * 1024;
};
