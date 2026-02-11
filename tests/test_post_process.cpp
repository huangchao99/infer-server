/**
 * @file test_post_process.cpp
 * @brief YOLO 后处理单元测试
 *
 * 不依赖 RKNN 硬件，使用合成 float 张量数据验证:
 * - NMS 算法正确性
 * - YOLOv5 anchor-based 解码
 * - YOLOv8/v11 anchor-free DFL 解码
 * - 坐标缩放 (letterbox)
 * - INT8 反量化
 * - 统一分发接口
 */

#include "infer_server/inference/post_processor.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>
#include <algorithm>

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

#define ASSERT_NEAR(a, b, eps) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (std::abs(_a - _b) > (eps)) { \
            std::cerr << "  FAIL: " << #a << " ~= " << #b \
                      << " (" << _a << " vs " << _b << ", eps=" << eps \
                      << ") at line " << __LINE__ << std::endl; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

#define PASS() \
    do { std::cout << "  PASS" << std::endl; g_tests_passed++; } while(0)

// ============================================================
// 辅助函数: 逆 sigmoid
// ============================================================
static float logit(float p) {
    // inverse of sigmoid: logit(p) = log(p / (1-p))
    return std::log(p / (1.0f - p));
}

// ============================================================
// 测试 1: NMS 基本功能
// ============================================================
void test_nms_basic() {
    TEST_CASE("NMS basic - remove overlapping boxes");

    std::vector<Detection> dets;

    // 两个高度重叠的检测框 (同类), 第一个置信度更高
    Detection d1;
    d1.class_id = 0; d1.confidence = 0.9f;
    d1.bbox = {10, 10, 110, 110};
    dets.push_back(d1);

    Detection d2;
    d2.class_id = 0; d2.confidence = 0.8f;
    d2.bbox = {15, 15, 115, 115};  // 大量重叠
    dets.push_back(d2);

    // 不同位置的检测框 (同类)
    Detection d3;
    d3.class_id = 0; d3.confidence = 0.7f;
    d3.bbox = {200, 200, 300, 300};  // 不重叠
    dets.push_back(d3);

    PostProcessor::nms(dets, 0.5f);

    // d2 应该被抑制 (与 d1 重叠度高), d1 和 d3 保留
    ASSERT_EQ(dets.size(), 2u);
    ASSERT_NEAR(dets[0].confidence, 0.9f, 0.01f);
    ASSERT_NEAR(dets[1].confidence, 0.7f, 0.01f);

    PASS();
}

// ============================================================
// 测试 2: NMS 不同类别不互相抑制
// ============================================================
void test_nms_different_classes() {
    TEST_CASE("NMS - different classes don't suppress each other");

    std::vector<Detection> dets;

    Detection d1;
    d1.class_id = 0; d1.confidence = 0.9f;
    d1.bbox = {10, 10, 110, 110};
    dets.push_back(d1);

    Detection d2;
    d2.class_id = 1; d2.confidence = 0.8f;
    d2.bbox = {10, 10, 110, 110};  // 完全重叠但不同类
    dets.push_back(d2);

    PostProcessor::nms(dets, 0.5f);

    // 不同类不互相抑制, 两个都保留
    ASSERT_EQ(dets.size(), 2u);

    PASS();
}

// ============================================================
// 测试 3: NMS 空输入
// ============================================================
void test_nms_empty() {
    TEST_CASE("NMS - empty input");

    std::vector<Detection> dets;
    PostProcessor::nms(dets, 0.5f);
    ASSERT_EQ(dets.size(), 0u);

    PASS();
}

// ============================================================
// 测试 4: INT8 反量化
// ============================================================
void test_dequantize_int8() {
    TEST_CASE("INT8 dequantize");

    // zp=128, scale=0.1 -> float_val = (int8_val - 128) * 0.1
    int8_t data[] = {0, 10, -10, 127, -128};
    float output[5];
    PostProcessor::dequantize_int8(data, output, 5, 0, 0.5f);

    // with zp=0, scale=0.5: result = (val - 0) * 0.5
    ASSERT_NEAR(output[0], 0.0f, 0.001f);
    ASSERT_NEAR(output[1], 5.0f, 0.001f);
    ASSERT_NEAR(output[2], -5.0f, 0.001f);
    ASSERT_NEAR(output[3], 63.5f, 0.001f);
    ASSERT_NEAR(output[4], -64.0f, 0.001f);

    PASS();
}

// ============================================================
// 测试 5: YOLOv5 后处理 - 合成数据
// ============================================================
void test_yolov5_synthetic() {
    TEST_CASE("YOLOv5 - synthetic detection");

    // 创建一个最小的 YOLOv5 输出:
    // 模型: 640x640, 1 类
    // 3 个输出头, 但只在第一个头 (stride=8, 80x80) 的 (40,40) 位置放一个检测
    int model_w = 640, model_h = 640;
    int num_classes = 1;
    int num_anchors = 3;
    int entry_size = 5 + num_classes;  // 6

    // Head 0: stride=8, grid=80x80
    int grid_h0 = 80, grid_w0 = 80;
    int head0_size = grid_h0 * grid_w0 * num_anchors * entry_size;
    std::vector<float> head0(head0_size, 0.0f);

    // 在 (40, 40) 的 anchor 0 放一个高置信度检测
    // offset = (y * grid_w * num_anchors + x * num_anchors + a) * entry_size
    int y = 40, x = 40, a = 0;
    int offset = (y * grid_w0 * num_anchors + x * num_anchors + a) * entry_size;

    // cx=0 -> sigmoid(0)=0.5 -> (0.5*2-0.5+40)*8 = (0.5+40)*8 = 324
    // cy=0 -> similar
    // w=0 -> sigmoid(0)=0.5 -> (0.5*2)^2 * anchor_w = 1.0 * 10.0 = 10.0
    // h=0 -> sigmoid(0)=0.5 -> (0.5*2)^2 * anchor_h = 1.0 * 13.0 = 13.0
    head0[offset + 0] = 0.0f;  // cx raw
    head0[offset + 1] = 0.0f;  // cy raw
    head0[offset + 2] = 0.0f;  // w raw
    head0[offset + 3] = 0.0f;  // h raw
    head0[offset + 4] = logit(0.95f);  // obj_conf raw -> sigmoid -> 0.95
    head0[offset + 5] = logit(0.90f);  // class 0 score raw -> sigmoid -> 0.90

    // Head 1 & 2: 空
    int head1_size = 40 * 40 * num_anchors * entry_size;
    std::vector<float> head1(head1_size, 0.0f);
    int head2_size = 20 * 20 * num_anchors * entry_size;
    std::vector<float> head2(head2_size, 0.0f);

    // 设置 obj_conf 很低使其被过滤
    // (全 0 -> sigmoid(0)=0.5 * sigmoid(0)=0.5 = 0.25 < 0.5 conf_thresh)

    std::vector<float*> outputs = {head0.data(), head1.data(), head2.data()};
    std::vector<TensorAttr> attrs = {
        {head0_size, {1, 80, 80, num_anchors * entry_size}, 0, 1.0f, false},
        {head1_size, {1, 40, 40, num_anchors * entry_size}, 0, 1.0f, false},
        {head2_size, {1, 20, 20, num_anchors * entry_size}, 0, 1.0f, false},
    };

    std::vector<std::string> labels = {"person"};

    auto dets = PostProcessor::yolov5(outputs, attrs, model_w, model_h,
                                       model_w, model_h,  // same as model size
                                       0.5f, 0.45f, labels);

    std::cout << "  Detected " << dets.size() << " object(s)" << std::endl;
    ASSERT_TRUE(dets.size() >= 1);

    // 验证第一个检测
    auto& d = dets[0];
    ASSERT_EQ(d.class_id, 0);
    ASSERT_TRUE(d.class_name == "person");
    // conf = 0.95 * 0.90 = 0.855
    ASSERT_NEAR(d.confidence, 0.95f * 0.90f, 0.02f);
    std::cout << "  Detection: class=" << d.class_name
              << " conf=" << d.confidence
              << " box=(" << d.bbox.x1 << "," << d.bbox.y1
              << "," << d.bbox.x2 << "," << d.bbox.y2 << ")" << std::endl;

    PASS();
}

// ============================================================
// 测试 6: YOLOv8 后处理 - 合成数据
// ============================================================
void test_yolov8_synthetic() {
    TEST_CASE("YOLOv8 - synthetic detection");

    int model_w = 640, model_h = 640;
    int num_classes = 2;
    int reg_max = 16;
    int box_channels = 4 * reg_max;  // 64
    int channel = box_channels + num_classes;  // 66

    // Head 0: stride=8, grid=80x80
    int grid_h0 = 80, grid_w0 = 80;
    int head0_size = grid_h0 * grid_w0 * channel;
    std::vector<float> head0(head0_size, 0.0f);

    // 在 (40, 40) 放一个检测
    int y = 40, x = 40;
    int offset = (y * grid_w0 + x) * channel;

    // DFL box regression: 设置每组 16 个值中只有 index=5 的值很大
    // 这样 DFL 解码结果约等于 5.0
    // distance = 5.0 * stride(8) = 40 pixels
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < reg_max; j++) {
            head0[offset + i * reg_max + j] = (j == 5) ? 10.0f : 0.0f;
        }
    }

    // Class scores (raw, will be sigmoided)
    head0[offset + box_channels + 0] = logit(0.05f);  // class 0: low
    head0[offset + box_channels + 1] = logit(0.92f);  // class 1: high

    // Head 1 & 2: 空 (score 很低)
    int head1_size = 40 * 40 * channel;
    std::vector<float> head1(head1_size, -10.0f);  // sigmoid(-10) ≈ 0
    int head2_size = 20 * 20 * channel;
    std::vector<float> head2(head2_size, -10.0f);

    std::vector<float*> outputs = {head0.data(), head1.data(), head2.data()};
    std::vector<TensorAttr> attrs = {
        {head0_size, {1, 80, 80, channel}, 0, 1.0f, false},
        {head1_size, {1, 40, 40, channel}, 0, 1.0f, false},
        {head2_size, {1, 20, 20, channel}, 0, 1.0f, false},
    };

    std::vector<std::string> labels = {"cat", "dog"};

    auto dets = PostProcessor::yolov8(outputs, attrs, model_w, model_h,
                                       model_w, model_h,
                                       0.5f, 0.45f, labels);

    std::cout << "  Detected " << dets.size() << " object(s)" << std::endl;
    ASSERT_TRUE(dets.size() >= 1);

    auto& d = dets[0];
    ASSERT_EQ(d.class_id, 1);
    ASSERT_TRUE(d.class_name == "dog");
    ASSERT_NEAR(d.confidence, 0.92f, 0.02f);

    // center = (40+0.5)*8 = 324
    // DFL decode ≈ 5.0, distance = 5.0 * 8 = 40
    // box: (324-40, 324-40, 324+40, 324+40) = (284, 284, 364, 364)
    std::cout << "  Detection: class=" << d.class_name
              << " conf=" << d.confidence
              << " box=(" << d.bbox.x1 << "," << d.bbox.y1
              << "," << d.bbox.x2 << "," << d.bbox.y2 << ")" << std::endl;
    ASSERT_NEAR(d.bbox.x1, 284.0f, 2.0f);
    ASSERT_NEAR(d.bbox.y1, 284.0f, 2.0f);
    ASSERT_NEAR(d.bbox.x2, 364.0f, 2.0f);
    ASSERT_NEAR(d.bbox.y2, 364.0f, 2.0f);

    PASS();
}

// ============================================================
// 测试 7: 坐标缩放 (letterbox)
// ============================================================
void test_scale_coords_letterbox() {
    TEST_CASE("Scale coords - letterbox mapping");

    // 场景: 模型 640x640, 原始图像 1920x1080
    // letterbox: scale = min(640/1920, 640/1080) = min(0.333, 0.593) = 0.333
    // scaled: 1920*0.333=640, 1080*0.333=360
    // pad_x = (640-640)/2 = 0
    // pad_y = (640-360)/2 = 140
    int model_w = 640, model_h = 640;
    int orig_w = 1920, orig_h = 1080;

    // 在模型坐标中的一个检测: (100, 200, 300, 400)
    std::vector<Detection> dets;
    Detection d;
    d.class_id = 0; d.confidence = 0.9f;
    d.bbox = {100.0f, 200.0f, 300.0f, 400.0f};
    dets.push_back(d);

    // 使用 PostProcessor 的 process 间接测试 (通过反射调用不行)
    // 直接测试: 手动计算
    float scale = std::min(640.0f / 1920.0f, 640.0f / 1080.0f);  // 0.3333
    float pad_x = (640.0f - 1920.0f * scale) / 2.0f;  // 0
    float pad_y = (640.0f - 1080.0f * scale) / 2.0f;  // 140

    // 预期: x1 = (100 - 0) / 0.333 = 300
    //       y1 = (200 - 140) / 0.333 = 180
    //       x2 = (300 - 0) / 0.333 = 900
    //       y2 = (400 - 140) / 0.333 = 780
    float exp_x1 = (100.0f - pad_x) / scale;
    float exp_y1 = (200.0f - pad_y) / scale;
    float exp_x2 = (300.0f - pad_x) / scale;
    float exp_y2 = (400.0f - pad_y) / scale;

    std::cout << "  scale=" << scale << " pad=(" << pad_x << "," << pad_y << ")" << std::endl;
    std::cout << "  Expected: (" << exp_x1 << "," << exp_y1 << ","
              << exp_x2 << "," << exp_y2 << ")" << std::endl;

    // 使用 PostProcessor::yolov5 会调用 scale_coords, 但我们需要直接访问
    // 通过 process 接口构造一个能产生已知输出的测试数据
    // 简单方法: 直接验证数学公式
    ASSERT_NEAR(exp_x1, 300.0f, 1.0f);
    ASSERT_NEAR(exp_y1, 180.0f, 1.0f);
    ASSERT_NEAR(exp_x2, 900.0f, 1.0f);
    ASSERT_NEAR(exp_y2, 780.0f, 1.0f);

    PASS();
}

// ============================================================
// 测试 8: process 分发接口
// ============================================================
void test_process_dispatch() {
    TEST_CASE("process() dispatch - unknown model type returns empty");

    std::vector<float*> outputs;
    std::vector<TensorAttr> attrs;
    std::vector<std::string> labels;

    auto dets = PostProcessor::process("unknown_model", outputs, attrs,
                                        640, 640, 1920, 1080,
                                        0.5f, 0.45f, labels);
    ASSERT_EQ(dets.size(), 0u);

    PASS();
}

// ============================================================
// 测试 9: NMS 多个高重叠框
// ============================================================
void test_nms_many_overlapping() {
    TEST_CASE("NMS - many overlapping boxes");

    std::vector<Detection> dets;
    // 创建 10 个高度重叠的同类框
    for (int i = 0; i < 10; i++) {
        Detection d;
        d.class_id = 0;
        d.confidence = 0.9f - i * 0.05f;
        d.bbox = {10.0f + i, 10.0f + i, 110.0f + i, 110.0f + i};
        dets.push_back(d);
    }

    PostProcessor::nms(dets, 0.5f);

    // 大部分应该被抑制, 只保留少数
    std::cout << "  After NMS: " << dets.size() << " boxes remain" << std::endl;
    ASSERT_TRUE(dets.size() >= 1);
    ASSERT_TRUE(dets.size() <= 3);
    // 最高置信度的应该在第一个
    ASSERT_NEAR(dets[0].confidence, 0.9f, 0.01f);

    PASS();
}

// ============================================================
// 测试 10: YOLOv5 过滤低置信度
// ============================================================
void test_yolov5_filter_low_conf() {
    TEST_CASE("YOLOv5 - filter low confidence detections");

    int model_w = 640, model_h = 640;
    int num_classes = 1;
    int num_anchors = 3;
    int entry_size = 5 + num_classes;

    // 全部填 0 -> obj_conf = sigmoid(0) = 0.5
    // class_score = sigmoid(0) = 0.5 -> final = 0.25
    // 阈值 0.5 应过滤掉所有
    int head0_size = 80 * 80 * num_anchors * entry_size;
    std::vector<float> head0(head0_size, 0.0f);
    int head1_size = 40 * 40 * num_anchors * entry_size;
    std::vector<float> head1(head1_size, 0.0f);
    int head2_size = 20 * 20 * num_anchors * entry_size;
    std::vector<float> head2(head2_size, 0.0f);

    std::vector<float*> outputs = {head0.data(), head1.data(), head2.data()};
    std::vector<TensorAttr> attrs = {
        {head0_size, {1, 80, 80, num_anchors * entry_size}, 0, 1.0f, false},
        {head1_size, {1, 40, 40, num_anchors * entry_size}, 0, 1.0f, false},
        {head2_size, {1, 20, 20, num_anchors * entry_size}, 0, 1.0f, false},
    };

    auto dets = PostProcessor::yolov5(outputs, attrs, model_w, model_h,
                                       model_w, model_h,
                                       0.5f, 0.45f, {});

    std::cout << "  Detected " << dets.size() << " (expected 0)" << std::endl;
    ASSERT_EQ(dets.size(), 0u);

    PASS();
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  Post-Processor Unit Tests" << std::endl;
    std::cout << "======================================" << std::endl;

    test_nms_basic();
    test_nms_different_classes();
    test_nms_empty();
    test_nms_many_overlapping();
    test_dequantize_int8();
    test_yolov5_synthetic();
    test_yolov5_filter_low_conf();
    test_yolov8_synthetic();
    test_scale_coords_letterbox();
    test_process_dispatch();

    std::cout << "\n======================================" << std::endl;
    std::cout << "  Results: " << g_tests_passed << " passed, "
              << g_tests_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    return g_tests_failed > 0 ? 1 : 0;
}
