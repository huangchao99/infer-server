/**
 * @file test_bounded_queue.cpp
 * @brief BoundedQueue 有界队列综合测试
 *
 * 测试内容:
 *   1. 基本 push/pop (FIFO 顺序)
 *   2. 容量限制与丢弃策略 (丢弃最旧)
 *   3. 丢弃计数统计
 *   4. try_pop 空队列返回 nullopt
 *   5. pop 超时返回 nullopt
 *   6. stop() 唤醒阻塞的 pop
 *   7. push 到已停止的队列返回 false
 *   8. reset 重置状态
 *   9. 多线程生产者/消费者 (正确性验证)
 *  10. 移动语义类型支持 (unique_ptr)
 *
 * 编译: cmake --build build --target test_bounded_queue
 * 运行: ./build/tests/test_bounded_queue
 */

#include "infer_server/common/bounded_queue.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>
#include <cassert>
#include <numeric>
#include <algorithm>
#include <set>

// ============================================================
// 简易测试框架
// ============================================================

struct TestCase {
    std::string name;
    std::function<void()> func;
};

static std::vector<TestCase> g_tests;
static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond)                                                      \
    do {                                                                        \
        if (!(cond)) {                                                          \
            throw std::runtime_error(                                           \
                std::string("ASSERT_TRUE failed: ") + #cond +                  \
                " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")");        \
        }                                                                       \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                        \
        auto _a = (a); auto _b = (b);                                          \
        if (_a != _b) {                                                         \
            throw std::runtime_error(                                           \
                std::string("ASSERT_EQ failed: ") + #a + "=" +                 \
                std::to_string(_a) + " != " + #b + "=" +                       \
                std::to_string(_b) +                                            \
                " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")");        \
        }                                                                       \
    } while (0)

#define TEST(test_name)                                                        \
    static void test_fn_##test_name();                                         \
    static bool _reg_##test_name = [] {                                        \
        g_tests.push_back({#test_name, test_fn_##test_name});                  \
        return true;                                                            \
    }();                                                                        \
    static void test_fn_##test_name()

// ============================================================
// 测试用例
// ============================================================

using infer_server::BoundedQueue;

// 1. 基本 push/pop - FIFO 顺序
TEST(basic_push_pop) {
    BoundedQueue<int> q(10);

    ASSERT_TRUE(q.empty());
    ASSERT_EQ(q.size(), 0u);
    ASSERT_EQ(q.capacity(), 10u);

    // push 3 个元素
    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.push(2));
    ASSERT_TRUE(q.push(3));

    ASSERT_EQ(q.size(), 3u);
    ASSERT_FALSE(q.empty());
    ASSERT_FALSE(q.full());

    // pop 验证 FIFO 顺序
    auto v1 = q.pop(std::chrono::milliseconds(100));
    auto v2 = q.pop(std::chrono::milliseconds(100));
    auto v3 = q.pop(std::chrono::milliseconds(100));

    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v3.has_value());
    ASSERT_EQ(v1.value(), 1);
    ASSERT_EQ(v2.value(), 2);
    ASSERT_EQ(v3.value(), 3);

    ASSERT_TRUE(q.empty());
    ASSERT_EQ(q.dropped_count(), 0u);
}

// 2. 容量限制与丢弃策略
TEST(capacity_and_drop_oldest) {
    BoundedQueue<int> q(3);

    // 填满队列
    q.push(1);
    q.push(2);
    q.push(3);
    ASSERT_TRUE(q.full());
    ASSERT_EQ(q.size(), 3u);

    // 继续 push, 应丢弃最旧的 (1)
    q.push(4);
    ASSERT_EQ(q.size(), 3u);

    // 再 push, 丢弃 2
    q.push(5);
    ASSERT_EQ(q.size(), 3u);

    // 此时队列应为: [3, 4, 5]
    auto v1 = q.try_pop();
    auto v2 = q.try_pop();
    auto v3 = q.try_pop();

    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v3.has_value());
    ASSERT_EQ(v1.value(), 3);
    ASSERT_EQ(v2.value(), 4);
    ASSERT_EQ(v3.value(), 5);
}

// 3. 丢弃计数统计
TEST(dropped_count) {
    BoundedQueue<int> q(2);

    q.push(1);
    q.push(2);
    ASSERT_EQ(q.dropped_count(), 0u);

    q.push(3);  // 丢弃 1
    ASSERT_EQ(q.dropped_count(), 1u);

    q.push(4);  // 丢弃 2
    ASSERT_EQ(q.dropped_count(), 2u);

    q.push(5);  // 丢弃 3
    q.push(6);  // 丢弃 4
    ASSERT_EQ(q.dropped_count(), 4u);
}

// 4. try_pop 空队列
TEST(try_pop_empty) {
    BoundedQueue<int> q(5);

    auto result = q.try_pop();
    ASSERT_FALSE(result.has_value());
}

// 5. pop 超时
TEST(pop_timeout) {
    BoundedQueue<int> q(5);

    auto start = std::chrono::steady_clock::now();
    auto result = q.pop(std::chrono::milliseconds(200));
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    ASSERT_FALSE(result.has_value());
    // 至少等待了 180ms (允许一些调度偏差)
    ASSERT_TRUE(ms >= 150);
    // 但不应该等太久 (不超过 500ms)
    ASSERT_TRUE(ms < 500);
}

// 6. stop() 唤醒阻塞的 pop
TEST(stop_unblocks_pop) {
    BoundedQueue<int> q(5);
    std::atomic<bool> popped{false};
    std::optional<int> pop_result;

    // 启动一个线程等待 pop (长超时)
    std::thread consumer([&] {
        pop_result = q.pop(std::chrono::milliseconds(5000));
        popped = true;
    });

    // 给线程时间进入等待状态
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_FALSE(popped.load());

    // 停止队列, 应该唤醒 consumer
    q.stop();

    // 等待 consumer 完成
    consumer.join();

    ASSERT_TRUE(popped.load());
    ASSERT_FALSE(pop_result.has_value());  // 队列为空, 应返回 nullopt
    ASSERT_TRUE(q.is_stopped());
}

// 7. push 到已停止的队列
TEST(push_after_stop) {
    BoundedQueue<int> q(5);
    q.push(1);
    q.stop();

    // push 应返回 false
    ASSERT_FALSE(q.push(2));
    // 已有的元素仍然可以 pop
    auto result = q.try_pop();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value(), 1);
}

// 8. reset 重置状态
TEST(reset_state) {
    BoundedQueue<int> q(3);

    q.push(1);
    q.push(2);
    q.push(3);
    q.push(4);  // 丢弃 1
    ASSERT_EQ(q.dropped_count(), 1u);
    ASSERT_EQ(q.size(), 3u);

    q.stop();
    ASSERT_TRUE(q.is_stopped());

    // reset 应清空所有状态
    q.reset();
    ASSERT_FALSE(q.is_stopped());
    ASSERT_EQ(q.size(), 0u);
    ASSERT_EQ(q.dropped_count(), 0u);
    ASSERT_TRUE(q.empty());

    // 应该可以重新使用
    ASSERT_TRUE(q.push(10));
    auto result = q.try_pop();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value(), 10);
}

// 9. 多线程生产者/消费者
TEST(multithreaded_producer_consumer) {
    const size_t QUEUE_SIZE = 8;
    const int NUM_PRODUCERS = 4;
    const int ITEMS_PER_PRODUCER = 1000;
    const int NUM_CONSUMERS = 2;

    BoundedQueue<int> q(QUEUE_SIZE);
    std::atomic<int> total_consumed{0};
    std::atomic<bool> producing_done{false};

    // 用于验证: 记录所有被消费的值
    std::vector<std::vector<int>> consumed_items(NUM_CONSUMERS);

    // 生产者线程
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; p++) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
                int value = p * ITEMS_PER_PRODUCER + i;
                q.push(value);
                // 模拟不同速度的生产者
                if (i % 100 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 消费者线程
    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&, c] {
            while (true) {
                auto item = q.pop(std::chrono::milliseconds(100));
                if (item.has_value()) {
                    consumed_items[c].push_back(item.value());
                    total_consumed++;
                } else if (producing_done.load() && q.empty()) {
                    // 生产结束且队列为空, 再等一下确认
                    item = q.pop(std::chrono::milliseconds(50));
                    if (item.has_value()) {
                        consumed_items[c].push_back(item.value());
                        total_consumed++;
                    } else {
                        break;
                    }
                }
            }
        });
    }

    // 等待所有生产者完成
    for (auto& t : producers) t.join();
    producing_done = true;

    // 等待所有消费者完成
    for (auto& t : consumers) t.join();

    // 验证
    size_t total_produced = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    size_t total_items = static_cast<size_t>(total_consumed.load());
    size_t total_dropped = q.dropped_count();

    std::cout << "    Produced:  " << total_produced << std::endl;
    std::cout << "    Consumed:  " << total_items << std::endl;
    std::cout << "    Dropped:   " << total_dropped << std::endl;
    std::cout << "    Sum check: " << (total_items + total_dropped) << " (should be " << total_produced << ")" << std::endl;

    // 消费 + 丢弃 = 生产
    ASSERT_EQ(total_items + total_dropped, total_produced);

    // 确保没有重复 (所有消费的值应该唯一)
    std::set<int> all_consumed;
    for (const auto& items : consumed_items) {
        for (int v : items) {
            ASSERT_TRUE(all_consumed.insert(v).second);  // insert 返回 {iter, success}
        }
    }
    ASSERT_EQ(all_consumed.size(), total_items);
}

// 10. 移动语义类型支持 (unique_ptr)
TEST(move_only_type) {
    BoundedQueue<std::unique_ptr<int>> q(3);

    q.push(std::make_unique<int>(42));
    q.push(std::make_unique<int>(100));

    auto v1 = q.pop(std::chrono::milliseconds(100));
    ASSERT_TRUE(v1.has_value());
    ASSERT_EQ(*v1.value(), 42);

    auto v2 = q.try_pop();
    ASSERT_TRUE(v2.has_value());
    ASSERT_EQ(*v2.value(), 100);

    // 测试溢出丢弃
    q.push(std::make_unique<int>(1));
    q.push(std::make_unique<int>(2));
    q.push(std::make_unique<int>(3));
    q.push(std::make_unique<int>(4));  // 丢弃 1

    ASSERT_EQ(q.dropped_count(), 1u);
    auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(*v.value(), 2);
}

// 11. 容量为 1 的边界情况
TEST(capacity_one) {
    BoundedQueue<std::string> q(1);

    q.push("hello");
    ASSERT_TRUE(q.full());
    ASSERT_EQ(q.size(), 1u);

    q.push("world");  // 丢弃 "hello"
    ASSERT_EQ(q.size(), 1u);
    ASSERT_EQ(q.dropped_count(), 1u);

    auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(v.value() == "world");  // 字符串比较用 ASSERT_TRUE
}

// 12. 多生产者高压力测试 (测试竞争条件)
TEST(stress_test) {
    const size_t QUEUE_SIZE = 4;
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 5000;

    BoundedQueue<int> q(QUEUE_SIZE);
    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};

    std::vector<std::thread> threads;

    // 每个线程同时做 push 和 pop
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                if (i % 2 == 0) {
                    if (q.push(t * OPS_PER_THREAD + i)) {
                        push_count++;
                    }
                } else {
                    auto v = q.try_pop();
                    if (v.has_value()) {
                        pop_count++;
                    }
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    // 清空队列中剩余的元素
    while (auto v = q.try_pop()) {
        pop_count++;
    }

    size_t total = static_cast<size_t>(push_count.load());
    size_t consumed = static_cast<size_t>(pop_count.load());
    size_t dropped = q.dropped_count();

    std::cout << "    Pushed:  " << total << std::endl;
    std::cout << "    Popped:  " << consumed << std::endl;
    std::cout << "    Dropped: " << dropped << std::endl;

    // 消费 + 丢弃 = 推入
    ASSERT_EQ(consumed + dropped, total);
}

// ============================================================
// 测试运行器
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  BoundedQueue Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    for (auto& tc : g_tests) {
        std::cout << "[RUN ] " << tc.name << std::endl;
        auto start = std::chrono::steady_clock::now();
        try {
            tc.func();
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            std::cout << "[PASS] " << tc.name << " (" << ms << "ms)" << std::endl;
            g_pass++;
        } catch (const std::exception& e) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            std::cout << "[FAIL] " << tc.name << " (" << ms << "ms)" << std::endl;
            std::cout << "       " << e.what() << std::endl;
            g_fail++;
        }
        std::cout << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed"
              << " (total " << (g_pass + g_fail) << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    return g_fail > 0 ? 1 : 0;
}
