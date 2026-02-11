/**
 * @file test_image_cache.cpp
 * @brief 图片缓存 (ImageCache) 单元测试
 *
 * 不需要硬件, 使用合成数据测试。可在任何平台上运行。
 *
 * 测试内容:
 *   1. 基本添加和获取帧
 *   2. 精确时间戳查询
 *   3. 最近时间戳查询
 *   4. 获取最新帧
 *   5. 过期帧自动淘汰
 *   6. 全局内存限制淘汰
 *   7. 流的添加和删除
 *   8. 多流并发写入
 *   9. 内存统计准确性
 */

#include "infer_server/cache/image_cache.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <functional>
#include <chrono>
#include <cassert>

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

using infer_server::ImageCache;
using infer_server::CachedFrame;

/// 创建合成测试帧
static CachedFrame make_frame(const std::string& cam_id, uint64_t frame_id,
                              int64_t ts_ms, size_t jpeg_size = 1024)
{
    CachedFrame f;
    f.cam_id = cam_id;
    f.frame_id = frame_id;
    f.timestamp_ms = ts_ms;
    f.width = 640;
    f.height = 360;
    f.jpeg_data = std::make_shared<std::vector<uint8_t>>(jpeg_size, 0xFF);
    return f;
}

// ============================================================
// 测试用例
// ============================================================

// 1. 基本添加和获取
TEST(basic_add_get) {
    ImageCache cache(5, 0);  // 5秒, 不限内存

    auto frame = make_frame("cam01", 1, 1000);
    cache.add_frame(frame);

    ASSERT_EQ(cache.total_frames(), 1u);
    ASSERT_EQ(cache.stream_count(), 1u);

    auto result = cache.get_frame("cam01", 1000);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->frame_id, 1u);
    ASSERT_EQ(result->timestamp_ms, 1000);
}

// 2. 精确时间戳查询
TEST(exact_timestamp_lookup) {
    ImageCache cache(10, 0);

    cache.add_frame(make_frame("cam01", 1, 1000));
    cache.add_frame(make_frame("cam01", 2, 1200));
    cache.add_frame(make_frame("cam01", 3, 1400));

    auto r1 = cache.get_frame("cam01", 1200);
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1->frame_id, 2u);

    // 不存在的时间戳
    auto r2 = cache.get_frame("cam01", 1100);
    ASSERT_FALSE(r2.has_value());

    // 不存在的流
    auto r3 = cache.get_frame("cam99", 1000);
    ASSERT_FALSE(r3.has_value());
}

// 3. 最近时间戳查询
TEST(nearest_timestamp_lookup) {
    ImageCache cache(10, 0);

    cache.add_frame(make_frame("cam01", 1, 1000));
    cache.add_frame(make_frame("cam01", 2, 2000));
    cache.add_frame(make_frame("cam01", 3, 3000));

    // 最接近 1800 的是 2000 (差200) vs 1000 (差800)
    auto r1 = cache.get_nearest_frame("cam01", 1800);
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1->frame_id, 2u);

    // 最接近 2600 的是 3000 (差400) vs 2000 (差600)
    auto r2 = cache.get_nearest_frame("cam01", 2600);
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2->frame_id, 3u);

    // 空流
    auto r3 = cache.get_nearest_frame("cam99", 1000);
    ASSERT_FALSE(r3.has_value());
}

// 4. 获取最新帧
TEST(latest_frame) {
    ImageCache cache(10, 0);

    cache.add_frame(make_frame("cam01", 1, 1000));
    cache.add_frame(make_frame("cam01", 2, 2000));
    cache.add_frame(make_frame("cam01", 3, 3000));

    auto latest = cache.get_latest_frame("cam01");
    ASSERT_TRUE(latest.has_value());
    ASSERT_EQ(latest->frame_id, 3u);
    ASSERT_EQ(latest->timestamp_ms, 3000);
}

// 5. 过期帧自动淘汰
TEST(expire_old_frames) {
    ImageCache cache(2, 0);  // 只保留 2 秒

    // 添加帧, 时间跨度 5 秒 (1000~6000ms)
    for (int i = 0; i < 6; i++) {
        int64_t ts = 1000 + i * 1000;
        cache.add_frame(make_frame("cam01", i + 1, ts));
    }

    // 最新帧 ts=6000, 保留 2s 范围 = [4000, 6000]
    // 帧 1(1000), 2(2000), 3(3000) 应被淘汰
    // 帧 4(4000), 5(5000), 6(6000) 应保留
    size_t count = cache.stream_frame_count("cam01");
    std::cout << "    Frames after expire: " << count << " (expected 3)" << std::endl;
    ASSERT_EQ(count, 3u);

    // 验证旧帧已删除
    ASSERT_FALSE(cache.get_frame("cam01", 1000).has_value());
    ASSERT_FALSE(cache.get_frame("cam01", 2000).has_value());
    ASSERT_FALSE(cache.get_frame("cam01", 3000).has_value());

    // 验证新帧存在
    ASSERT_TRUE(cache.get_frame("cam01", 4000).has_value());
    ASSERT_TRUE(cache.get_frame("cam01", 5000).has_value());
    ASSERT_TRUE(cache.get_frame("cam01", 6000).has_value());
}

// 6. 全局内存限制淘汰
TEST(memory_limit_eviction) {
    // 限制 10KB
    ImageCache cache(60, 0);  // 60 秒 (不让时间淘汰干扰)
    // 手动设置 max_memory 为很小的值
    // 通过构造函数: max_memory_mb = 0 表示不限制, 所以这里用特殊方式

    // 使用一个小内存限制的缓存
    // 10KB = 0.00976 MB, 但构造函数用 int MB...
    // 改用大帧 + 1MB 限制来测试
    ImageCache cache2(60, 1);  // 60秒, 1MB 限制

    // 每帧 200KB, 添加 6 帧 = 1.2MB > 1MB
    for (int i = 0; i < 6; i++) {
        int64_t ts = static_cast<int64_t>(i + 1) * 1000;
        cache2.add_frame(make_frame("cam01", i + 1, ts, 200 * 1024));  // 200KB each
    }

    size_t mem = cache2.total_memory_bytes();
    std::cout << "    Memory after 6x200KB frames: " << (mem / 1024) << "KB "
              << "(limit 1024KB)" << std::endl;
    ASSERT_TRUE(mem <= 1024 * 1024);  // <= 1MB

    // 应该有些帧被淘汰了
    size_t frames = cache2.total_frames();
    std::cout << "    Frames remaining: " << frames << " (expected <= 5)" << std::endl;
    ASSERT_TRUE(frames <= 5);
}

// 7. 流的添加和删除
TEST(stream_add_remove) {
    ImageCache cache(5, 0);

    cache.add_frame(make_frame("cam01", 1, 1000));
    cache.add_frame(make_frame("cam02", 1, 1000));
    cache.add_frame(make_frame("cam03", 1, 1000));

    ASSERT_EQ(cache.stream_count(), 3u);
    ASSERT_EQ(cache.total_frames(), 3u);

    cache.remove_stream("cam02");
    ASSERT_EQ(cache.stream_count(), 2u);
    ASSERT_EQ(cache.total_frames(), 2u);
    ASSERT_FALSE(cache.get_latest_frame("cam02").has_value());

    // 删除不存在的流不应崩溃
    cache.remove_stream("cam99");
    ASSERT_EQ(cache.stream_count(), 2u);
}

// 8. 多流并发写入
TEST(concurrent_multi_stream) {
    ImageCache cache(10, 0);
    const int NUM_STREAMS = 4;
    const int FRAMES_PER_STREAM = 100;

    std::vector<std::thread> threads;
    for (int s = 0; s < NUM_STREAMS; s++) {
        threads.emplace_back([&cache, s] {
            std::string cam_id = "cam" + std::to_string(s);
            for (int i = 0; i < FRAMES_PER_STREAM; i++) {
                int64_t ts = static_cast<int64_t>(i) * 200;  // 200ms 间隔, 10秒内
                cache.add_frame(make_frame(cam_id, i + 1, ts, 512));
            }
        });
    }

    for (auto& t : threads) t.join();

    std::cout << "    Streams: " << cache.stream_count() << std::endl;
    std::cout << "    Total frames: " << cache.total_frames() << std::endl;
    std::cout << "    Total memory: " << (cache.total_memory_bytes() / 1024) << "KB" << std::endl;

    ASSERT_EQ(cache.stream_count(), static_cast<size_t>(NUM_STREAMS));

    // 每个流应该有帧 (不一定全部, 取决于时间淘汰)
    for (int s = 0; s < NUM_STREAMS; s++) {
        std::string cam_id = "cam" + std::to_string(s);
        ASSERT_TRUE(cache.stream_frame_count(cam_id) > 0);
        ASSERT_TRUE(cache.get_latest_frame(cam_id).has_value());
    }
}

// 9. 内存统计准确性
TEST(memory_accounting) {
    ImageCache cache(60, 0);  // 不让过期, 不限内存

    size_t frame_size = 5000;
    int num_frames = 10;

    for (int i = 0; i < num_frames; i++) {
        int64_t ts = static_cast<int64_t>(i) * 1000;
        cache.add_frame(make_frame("cam01", i + 1, ts, frame_size));
    }

    size_t expected_mem = frame_size * num_frames;
    size_t actual_mem = cache.total_memory_bytes();
    std::cout << "    Expected memory: " << expected_mem << " bytes" << std::endl;
    std::cout << "    Actual memory:   " << actual_mem << " bytes" << std::endl;
    ASSERT_EQ(actual_mem, expected_mem);

    // 删除流后内存应归零
    cache.remove_stream("cam01");
    ASSERT_EQ(cache.total_memory_bytes(), 0u);
}

// ============================================================
// 测试运行器
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  ImageCache Unit Tests" << std::endl;
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
