// ObjectPool.h —— 通用对象池（placement new + 显式析构）
#pragma once

#include <vector>
#include <mutex>

template<typename T>
class ObjectPool {
public:
    // 优先从池中取；参数转发给 T 的构造函数
    template<typename... Args>
    T* acquire(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            T* obj = pool_.back();
            pool_.pop_back();
            new (obj) T(std::forward<Args>(args)...);
            return obj;
        }
        return new T(std::forward<Args>(args)...);
    }

    // 归还对象：只析构不释放内存
    void release(T* obj) {
        if (!obj) return;
        obj->~T();
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push_back(obj);
    }

    // 清空池，释放所有内存
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* obj : pool_) {
            delete static_cast<void*>(obj);  // 只释放内存，不调用析构（已经调过了）
        }
        pool_.clear();
    }

    size_t size() const { return pool_.size(); }  // 仅用于调试

private:
    std::mutex mutex_;
    std::vector<T*> pool_;
};
