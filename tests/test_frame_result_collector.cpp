/**
 * @file test_frame_result_collector.cpp
 * @brief FrameResultCollector 单元测试
 *
 * 不依赖任何硬件, 验证:
 * - 单模型场景: 1 个模型完成即返回
 * - 多模型场景: 所有模型完成后返回
 * - 并发安全: 多线程同时 add_result
 * - 结果完整性: 所有 ModelResult 都被收集
 */

#include "infer_server/inference/frame_result_collector.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>

using namespace infer_server;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CASE(name) \
    do { std::cout << "\n[TEST] " << name << std::endl; } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "  FAIL: " << #expr << " at line " << __LINE__ << std::endl; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a != _b) { \
            std::cerr << "  FAIL: " << #a << " == " << #b \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ << std::endl; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

#define PASS() \
    do { std::cout << "  PASS" << std::endl; g_tests_passed++; } while(0)

// ============================================================
// 测试 1: 单模型场景
// ============================================================
void test_single_model() {
    TEST_CASE("Single model - immediate completion");

    FrameResult base;
    base.cam_id = "cam01";
    base.frame_id = 100;
    base.timestamp_ms = 1700000000000;
    base.original_width = 1920;
    base.original_height = 1080;

    FrameResultCollector collector(1, base);

    ASSERT_EQ(collector.total_models(), 1);
    ASSERT_EQ(collector.completed_count(), 0);
    ASSERT_TRUE(!collector.is_complete());

    ModelResult mr;
    mr.task_name = "phone_detection";
    mr.model_path = "/weights/yolo_phone.rknn";
    mr.inference_time_ms = 12.5;
    mr.detections.push_back(Detection{0, "phone", 0.95f, {100, 200, 300, 400}});

    auto result = collector.add_result(mr);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(collector.is_complete());
    ASSERT_EQ(collector.completed_count(), 1);

    // 验证结果
    ASSERT_TRUE(result->cam_id == "cam01");
    ASSERT_EQ(result->frame_id, 100u);
    ASSERT_EQ(result->results.size(), 1u);
    ASSERT_TRUE(result->results[0].task_name == "phone_detection");
    ASSERT_EQ(result->results[0].detections.size(), 1u);

    PASS();
}

// ============================================================
// 测试 2: 多模型场景 (串行)
// ============================================================
void test_multi_model_sequential() {
    TEST_CASE("Multi model sequential - completion on last");

    FrameResult base;
    base.cam_id = "cam02";
    base.frame_id = 200;

    FrameResultCollector collector(3, base);

    // 第 1 个模型
    ModelResult mr1;
    mr1.task_name = "phone";
    auto r1 = collector.add_result(mr1);
    ASSERT_TRUE(!r1.has_value());
    ASSERT_EQ(collector.completed_count(), 1);

    // 第 2 个模型
    ModelResult mr2;
    mr2.task_name = "smoking";
    auto r2 = collector.add_result(mr2);
    ASSERT_TRUE(!r2.has_value());
    ASSERT_EQ(collector.completed_count(), 2);

    // 第 3 个模型 -> 完成
    ModelResult mr3;
    mr3.task_name = "helmet";
    auto r3 = collector.add_result(mr3);
    ASSERT_TRUE(r3.has_value());
    ASSERT_TRUE(collector.is_complete());

    // 验证所有结果都在
    ASSERT_EQ(r3->results.size(), 3u);

    PASS();
}

// ============================================================
// 测试 3: 并发安全
// ============================================================
void test_concurrent_add_result() {
    TEST_CASE("Concurrent add_result - thread safety");

    const int NUM_MODELS = 8;

    FrameResult base;
    base.cam_id = "cam03";
    base.frame_id = 300;

    auto collector = std::make_shared<FrameResultCollector>(NUM_MODELS, base);

    std::atomic<int> complete_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_MODELS; i++) {
        threads.emplace_back([&, i]() {
            ModelResult mr;
            mr.task_name = "model_" + std::to_string(i);
            mr.inference_time_ms = 10.0 + i;

            // 稍微错开时间
            std::this_thread::sleep_for(std::chrono::microseconds(i * 100));

            auto result = collector->add_result(mr);
            if (result.has_value()) {
                complete_count++;
                // 只有一个线程应该得到完整结果
                ASSERT_EQ(result->results.size(), static_cast<size_t>(NUM_MODELS));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 恰好一个线程得到完整结果
    ASSERT_EQ(complete_count.load(), 1);
    ASSERT_TRUE(collector->is_complete());
    ASSERT_EQ(collector->completed_count(), NUM_MODELS);

    PASS();
}

// ============================================================
// 测试 4: 结果完整性
// ============================================================
void test_result_integrity() {
    TEST_CASE("Result integrity - all model results preserved");

    FrameResult base;
    base.cam_id = "cam04";
    base.rtsp_url = "rtsp://example.com/stream";
    base.frame_id = 400;
    base.timestamp_ms = 1700000005000;
    base.pts = 12345;
    base.original_width = 640;
    base.original_height = 360;

    FrameResultCollector collector(2, base);

    ModelResult mr1;
    mr1.task_name = "detect_phone";
    mr1.model_path = "/weight/phone.rknn";
    mr1.inference_time_ms = 8.2;
    mr1.detections.push_back(Detection{0, "phone", 0.88f, {50, 50, 200, 200}});
    mr1.detections.push_back(Detection{0, "phone", 0.72f, {300, 100, 450, 300}});

    ModelResult mr2;
    mr2.task_name = "detect_smoking";
    mr2.model_path = "/weight/smoking.rknn";
    mr2.inference_time_ms = 6.7;
    mr2.detections.push_back(Detection{0, "cigarette", 0.91f, {150, 80, 250, 180}});

    collector.add_result(mr1);
    auto result = collector.add_result(mr2);

    ASSERT_TRUE(result.has_value());

    // 验证基础信息保留
    ASSERT_TRUE(result->cam_id == "cam04");
    ASSERT_TRUE(result->rtsp_url == "rtsp://example.com/stream");
    ASSERT_EQ(result->frame_id, 400u);
    ASSERT_EQ(result->timestamp_ms, 1700000005000LL);
    ASSERT_EQ(result->pts, 12345LL);
    ASSERT_EQ(result->original_width, 640);
    ASSERT_EQ(result->original_height, 360);

    // 验证模型结果
    ASSERT_EQ(result->results.size(), 2u);

    // 检查总检测数
    size_t total_dets = 0;
    for (auto& mr : result->results) {
        total_dets += mr.detections.size();
    }
    ASSERT_EQ(total_dets, 3u);

    PASS();
}

// ============================================================
// 测试 5: shared_ptr 用法 (模拟 InferTask::aggregator)
// ============================================================
void test_shared_ptr_usage() {
    TEST_CASE("shared_ptr usage - simulating InferTask aggregator");

    FrameResult base;
    base.cam_id = "cam05";
    base.frame_id = 500;

    auto collector = std::make_shared<FrameResultCollector>(2, base);

    // 模拟 InferTask 中的 aggregator 存储
    std::shared_ptr<void> aggregator1 = collector;
    std::shared_ptr<void> aggregator2 = collector;

    // 模拟 InferWorker 中的使用
    auto* c1 = static_cast<FrameResultCollector*>(aggregator1.get());
    ModelResult mr1;
    mr1.task_name = "task_a";
    auto r1 = c1->add_result(mr1);
    ASSERT_TRUE(!r1.has_value());

    auto* c2 = static_cast<FrameResultCollector*>(aggregator2.get());
    ModelResult mr2;
    mr2.task_name = "task_b";
    auto r2 = c2->add_result(mr2);
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2->results.size(), 2u);

    PASS();
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  FrameResultCollector Unit Tests" << std::endl;
    std::cout << "======================================" << std::endl;

    test_single_model();
    test_multi_model_sequential();
    test_concurrent_add_result();
    test_result_integrity();
    test_shared_ptr_usage();

    std::cout << "\n======================================" << std::endl;
    std::cout << "  Results: " << g_tests_passed << " passed, "
              << g_tests_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    return g_tests_failed > 0 ? 1 : 0;
}
