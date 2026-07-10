cmake_minimum_required(VERSION 3.16)
project(TinyReactor VERSION 1.0.0 LANGUAGES CXX)

# ========== C++20 标准 ==========
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========== Windows 特定 ==========
if(WIN32)
    # 关闭警告：MSVC 的 C 运行时安全警告
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif()

# ========== 源文件 ==========
set(SOURCES
    main.cpp
    EventLoop.cpp
    TcpConnection.cpp
    Acceptor.cpp
    AsyncLogger.cpp
)

set(HEADERS
    EventLoop.h
    TcpConnection.h
    Acceptor.h
    ObjectPool.h
    AsyncLogger.h
)

# ========== 可执行文件 ==========
add_executable(TinyReactor ${SOURCES} ${HEADERS})

# ========== 链接库 ==========
target_link_libraries(TinyReactor PRIVATE ws2_32)

# ========== 编译选项 ==========
if(MSVC)
    target_compile_options(TinyReactor PRIVATE /W4 /utf-8)  # /W4: 高警告  /utf-8: UTF-8 源文件
else()
    target_compile_options(TinyReactor PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ========== Debug / Release ==========
target_compile_definitions(TinyReactor PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# ========== 安装 ==========
install(TARGETS TinyReactor RUNTIME DESTINATION bin)
