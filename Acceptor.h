// Acceptor.h —— TCP 监听器
#pragma once

#include "EventLoop.h"
#include "TcpConnection.h"
#include <functional>

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(SOCKET fd)>;

    Acceptor(EventLoop* loop, int port);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(const NewConnectionCallback& cb) {
        newConnectionCallback_ = cb;
    }

    void stop();

private:
    EventLoop* loop_;
    SOCKET listenFd_ = INVALID_SOCKET;
    TcpConnection* listenConn_ = nullptr;
    NewConnectionCallback newConnectionCallback_;
};
