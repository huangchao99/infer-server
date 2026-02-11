#pragma once

/**
 * @file bounded_queue.h
 * @brief 线程安全有界队列 (满时丢弃最旧元素)
 * 
 * 设计用于解码器→推理器之间的帧传递:
 * - 队列满时自动丢弃最旧的元素，保证实时性
 * - 支持阻塞 pop (带超时) 和非阻塞 try_pop
 * - 支持 stop() 优雅关闭
 * - 统计丢弃帧数
 * - 支持移动语义 (可用于 unique_ptr 等不可拷贝类型)
 */

#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <cstddef>

namespace infer_server {

template<typename T>
class BoundedQueue {
public:
    /// 构造有界队列
    /// @param capacity 最大容量 (必须 > 0)
    explicit BoundedQueue(size_t capacity)
        : capacity_(capacity > 0 ? capacity : 1) {}

    // 禁止拷贝
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // 允许移动
    BoundedQueue(BoundedQueue&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        capacity_ = other.capacity_;
        queue_ = std::move(other.queue_);
        stopped_ = other.stopped_;
        dropped_count_ = other.dropped_count_;
    }

    /// 向队列推入元素
    /// 如果队列已满，丢弃最旧的元素 (队首)
    /// @return true 如果成功推入, false 如果队列已停止
    bool push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return false;
            }
            if (queue_.size() >= capacity_) {
                queue_.pop();
                dropped_count_++;
            }
            queue_.push(std::move(item));
        }
        not_empty_cv_.notify_one();
        return true;
    }

    /// 阻塞弹出，带超时
    /// @param timeout 最大等待时间
    /// @return 弹出的元素, 如果超时或队列已停止则返回 nullopt
    std::optional<T> pop(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_cv_.wait_for(lock, timeout,
                [this] { return !queue_.empty() || stopped_; })) {
            return std::nullopt;  // 超时
        }
        if (queue_.empty()) {
            return std::nullopt;  // 被 stop() 唤醒但队列为空
        }
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    /// 非阻塞尝试弹出
    /// @return 弹出的元素, 如果队列为空则返回 nullopt
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    /// 当前队列大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /// 队列是否为空
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /// 队列是否已满
    bool full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size() >= capacity_;
    }

    /// 队列最大容量
    size_t capacity() const {
        return capacity_;
    }

    /// 累计丢弃的元素数量
    size_t dropped_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_count_;
    }

    /// 队列是否已停止
    bool is_stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

    /// 停止队列 (唤醒所有等待的 pop, 拒绝后续 push)
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        not_empty_cv_.notify_all();
    }

    /// 清空队列内容 (不改变 stopped 状态)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty_queue;
        queue_.swap(empty_queue);
    }

    /// 重置队列 (清空内容 + 取消停止状态 + 清零统计)
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty_queue;
        queue_.swap(empty_queue);
        stopped_ = false;
        dropped_count_ = 0;
    }

private:
    size_t capacity_;
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;
    bool stopped_ = false;
    size_t dropped_count_ = 0;
};

} // namespace infer_server
