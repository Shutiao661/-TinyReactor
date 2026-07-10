// Acceptor.cpp —— TCP 监听器实现（listenConn 标记为 listening，不走 recv）
#include "Acceptor.h"

#include <cassert>
#include <iostream>

Acceptor::Acceptor(EventLoop* loop, int port) : loop_(loop) {
    assert(loop_);

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ == INVALID_SOCKET) {
        std::cerr << "[Acceptor] socket() failed: " << WSAGetLastError() << "\n";
        abort();
    }

    // SO_REUSEADDR：支持快速重启
    int optval = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[Acceptor] bind(port=" << port << ") failed: " << WSAGetLastError() << "\n";
        closesocket(listenFd_);
        abort();
    }

    if (listen(listenFd_, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[Acceptor] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listenFd_);
        abort();
    }

    // 包装为 TcpConnection 并标记为 listening（handleRead 不 recv，直接回调）
    listenConn_ = new TcpConnection(loop_, listenFd_);
    listenConn_->setListening(true);
    listenConn_->setReadCallback([this](TcpConnection*, const std::string&) {
        if (newConnectionCallback_) {
            SOCKET clientFd = accept(listenFd_, nullptr, nullptr);
            if (clientFd != INVALID_SOCKET) {
                newConnectionCallback_(clientFd);
            }
        }
    });

    // queueInLoop: 构造在 main 线程，addConnection 必须在 baseLoop 线程
    loop_->queueInLoop([this]() {
        loop_->addConnection(listenConn_);
    });

    std::cout << "[Acceptor] listening on port " << port << "\n";
}

Acceptor::~Acceptor() {
    stop();
}

void Acceptor::stop() {
    if (!listenConn_) return;

    // listenConn_ 是 new 出来的，不在对象池中，不能走 shutdown/handleClose/release
    // 直接在 loop 线程中移除并 delete
    loop_->queueInLoop([conn = listenConn_] {
        conn->~TcpConnection();          // 显式析构（关闭 socket）
        // 不 release 到池，直接释放内存
        operator delete(conn);
    });
    listenConn_ = nullptr;
    listenFd_ = INVALID_SOCKET;
}
