/**
 * @file test_model_manager.cpp
 * @brief ModelManager 单元测试
 *
 * 需要 RKNN 硬件 (ARM 设备) + .rknn 模型文件。
 * 运行方式:
 *   sudo ./test_model_manager /path/to/model.rknn
 *
 * 测试内容:
 * - 模型加载
 * - IO 属性查询
 * - dup_context 创建
 * - 核心绑定
 * - 简单推理验证
 * - 模型卸载
 */

#ifdef HAS_RKNN

#include "infer_server/inference/model_manager.h"
#include "infer_server/common/logger.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>

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

static std::string g_model_path;

// ============================================================
// 测试 1: 模型加载
// ============================================================
void test_load_model() {
    TEST_CASE("Load RKNN model");

    ModelManager mgr;
    ASSERT_TRUE(!mgr.is_loaded(g_model_path));
    ASSERT_EQ(mgr.loaded_count(), 0u);

    bool ok = mgr.load_model(g_model_path);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(mgr.is_loaded(g_model_path));
    ASSERT_EQ(mgr.loaded_count(), 1u);

    // 重复加载应该返回 true (幂等)
    ok = mgr.load_model(g_model_path);
    ASSERT_TRUE(ok);
    ASSERT_EQ(mgr.loaded_count(), 1u);

    PASS();
}

// ============================================================
// 测试 2: 查询模型信息
// ============================================================
void test_model_info() {
    TEST_CASE("Query model info");

    ModelManager mgr;
    mgr.load_model(g_model_path);

    const ModelInfo* info = mgr.get_model_info(g_model_path);
    ASSERT_TRUE(info != nullptr);
    ASSERT_TRUE(info->io_num.n_input > 0);
    ASSERT_TRUE(info->io_num.n_output > 0);
    ASSERT_EQ(info->input_attrs.size(), static_cast<size_t>(info->io_num.n_input));
    ASSERT_EQ(info->output_attrs.size(), static_cast<size_t>(info->io_num.n_output));

    std::cout << "  Model: " << info->model_path << std::endl;
    std::cout << "  Inputs: " << info->io_num.n_input
              << ", Outputs: " << info->io_num.n_output << std::endl;

    for (size_t i = 0; i < info->input_attrs.size(); i++) {
        auto& a = info->input_attrs[i];
        std::cout << "  Input[" << i << "]: dims=[" << a.dims[0] << ","
                  << a.dims[1] << "," << a.dims[2] << "," << a.dims[3]
                  << "] n_elems=" << a.n_elems << std::endl;
    }

    for (size_t i = 0; i < info->output_attrs.size(); i++) {
        auto& a = info->output_attrs[i];
        std::cout << "  Output[" << i << "]: dims=[" << a.dims[0] << ","
                  << a.dims[1] << "," << a.dims[2] << "," << a.dims[3]
                  << "] n_elems=" << a.n_elems
                  << " type=" << a.type
                  << " zp=" << a.zp << " scale=" << a.scale << std::endl;
    }

    // 获取 PostProcessor 格式的属性
    auto tensor_attrs = info->get_output_tensor_attrs();
    ASSERT_EQ(tensor_attrs.size(), info->output_attrs.size());
    std::cout << "  TensorAttr conversion OK" << std::endl;

    // 不存在的模型
    ASSERT_TRUE(mgr.get_model_info("/nonexistent.rknn") == nullptr);

    PASS();
}

// ============================================================
// 测试 3: 创建 worker context
// ============================================================
void test_create_worker_context() {
    TEST_CASE("Create worker contexts (dup_context + core binding)");

    ModelManager mgr;
    mgr.load_model(g_model_path);

    // 为 3 个 NPU 核心各创建一个 context
    std::vector<rknn_context> contexts;
    for (int i = 0; i < 3; i++) {
        int mask = NpuCoreMask::from_worker_id(i);
        rknn_context ctx = mgr.create_worker_context(g_model_path, mask);
        ASSERT_TRUE(ctx != 0);
        std::cout << "  Worker " << i << ": ctx=" << ctx
                  << " core_mask=" << mask << std::endl;
        contexts.push_back(ctx);
    }

    // 创建一个 AUTO 核心的 context
    rknn_context auto_ctx = mgr.create_worker_context(g_model_path, NpuCoreMask::AUTO);
    ASSERT_TRUE(auto_ctx != 0);
    std::cout << "  Worker AUTO: ctx=" << auto_ctx << std::endl;
    contexts.push_back(auto_ctx);

    // 释放所有 context
    for (auto ctx : contexts) {
        mgr.release_worker_context(ctx);
    }

    PASS();
}

// ============================================================
// 测试 4: Worker context 推理验证
// ============================================================
void test_worker_inference() {
    TEST_CASE("Worker context inference (dummy input)");

    ModelManager mgr;
    mgr.load_model(g_model_path);

    const ModelInfo* info = mgr.get_model_info(g_model_path);
    ASSERT_TRUE(info != nullptr);

    // 创建 worker context
    rknn_context ctx = mgr.create_worker_context(g_model_path, NpuCoreMask::CORE_0);
    ASSERT_TRUE(ctx != 0);

    // 准备输入 (全 0 数据)
    rknn_input inputs[1];
    std::memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = info->input_attrs[0].n_elems;
    inputs[0].pass_through = 0;

    std::vector<uint8_t> input_data(inputs[0].size, 0);
    inputs[0].buf = input_data.data();

    int ret = rknn_inputs_set(ctx, 1, inputs);
    ASSERT_TRUE(ret == RKNN_SUCC);

    // 推理
    auto t0 = std::chrono::steady_clock::now();
    ret = rknn_run(ctx, nullptr);
    auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(ret == RKNN_SUCC);

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "  Inference time: " << ms << " ms" << std::endl;

    // 获取输出
    std::vector<rknn_output> outputs(info->io_num.n_output);
    for (uint32_t i = 0; i < info->io_num.n_output; i++) {
        std::memset(&outputs[i], 0, sizeof(rknn_output));
        outputs[i].want_float = 1;
    }

    ret = rknn_outputs_get(ctx, info->io_num.n_output, outputs.data(), nullptr);
    ASSERT_TRUE(ret == RKNN_SUCC);

    // 打印前几个值
    for (uint32_t i = 0; i < info->io_num.n_output; i++) {
        float* data = static_cast<float*>(outputs[i].buf);
        std::cout << "  Output[" << i << "] first 5 values:";
        int count = std::min(5, static_cast<int>(info->output_attrs[i].n_elems));
        for (int j = 0; j < count; j++) {
            std::cout << " " << data[j];
        }
        std::cout << std::endl;
    }

    rknn_outputs_release(ctx, info->io_num.n_output, outputs.data());
    mgr.release_worker_context(ctx);

    PASS();
}

// ============================================================
// 测试 5: 模型卸载
// ============================================================
void test_unload_model() {
    TEST_CASE("Unload model");

    ModelManager mgr;
    mgr.load_model(g_model_path);
    ASSERT_EQ(mgr.loaded_count(), 1u);

    mgr.unload_model(g_model_path);
    ASSERT_EQ(mgr.loaded_count(), 0u);
    ASSERT_TRUE(!mgr.is_loaded(g_model_path));

    // 卸载不存在的模型不应出错
    mgr.unload_model("/nonexistent.rknn");
    ASSERT_EQ(mgr.loaded_count(), 0u);

    PASS();
}

// ============================================================
// 测试 6: 加载不存在的模型
// ============================================================
void test_load_nonexistent() {
    TEST_CASE("Load nonexistent model");

    ModelManager mgr;
    bool ok = mgr.load_model("/nonexistent/model.rknn");
    ASSERT_TRUE(!ok);
    ASSERT_EQ(mgr.loaded_count(), 0u);

    PASS();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: sudo " << argv[0] << " <model.rknn>" << std::endl;
        std::cerr << "Example: sudo ./test_model_manager /path/to/yolov5.rknn" << std::endl;
        return 1;
    }

    g_model_path = argv[1];
    logger::init("debug");

    std::cout << "======================================" << std::endl;
    std::cout << "  ModelManager Unit Tests" << std::endl;
    std::cout << "  Model: " << g_model_path << std::endl;
    std::cout << "======================================" << std::endl;

    test_load_model();
    test_model_info();
    test_create_worker_context();
    test_worker_inference();
    test_unload_model();
    test_load_nonexistent();

    std::cout << "\n======================================" << std::endl;
    std::cout << "  Results: " << g_tests_passed << " passed, "
              << g_tests_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    logger::shutdown();
    return g_tests_failed > 0 ? 1 : 0;
}

#else // !HAS_RKNN

#include <iostream>
int main() {
    std::cout << "RKNN not available, skipping ModelManager tests." << std::endl;
    std::cout << "Build with -DENABLE_RKNN=ON on ARM device." << std::endl;
    return 0;
}

#endif // HAS_RKNN
