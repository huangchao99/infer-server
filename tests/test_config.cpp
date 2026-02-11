/**
 * @file test_config.cpp
 * @brief 配置管理与 JSON 序列化测试
 *
 * 测试内容:
 *   1. ServerConfig 默认值
 *   2. ServerConfig JSON 序列化/反序列化
 *   3. StreamConfig JSON 序列化/反序列化
 *   4. ModelConfig 含可选字段
 *   5. 配置文件保存与加载
 *   6. 流配置持久化与恢复
 *   7. FrameResult JSON 序列化
 *   8. 错误处理 (文件不存在等)
 *
 * 编译: cmake --build build --target test_config
 * 运行: ./build/tests/test_config
 */

#include "infer_server/common/config.h"
#include "infer_server/common/types.h"

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ============================================================
// 简易测试框架 (同 test_bounded_queue)
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
        if ((a) != (b)) {                                                       \
            throw std::runtime_error(                                           \
                std::string("ASSERT_EQ failed: ") + #a + " != " + #b +        \
                " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")");        \
        }                                                                       \
    } while (0)

#define ASSERT_THROWS(expr)                                                    \
    do {                                                                        \
        bool threw = false;                                                     \
        try { expr; } catch (...) { threw = true; }                            \
        if (!threw) {                                                           \
            throw std::runtime_error(                                           \
                std::string("ASSERT_THROWS failed: ") + #expr +                \
                " did not throw"                                                \
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

// 测试用临时目录
static std::string g_test_dir;

static void setup_test_dir() {
    g_test_dir = "test_config_tmp";
    fs::create_directories(g_test_dir);
}

static void cleanup_test_dir() {
    fs::remove_all(g_test_dir);
}

using namespace infer_server;

// ============================================================
// 测试用例
// ============================================================

// 1. ServerConfig 默认值
TEST(server_config_defaults) {
    ServerConfig config;
    ASSERT_EQ(config.http_port, 8080);
    ASSERT_EQ(config.num_infer_workers, 3);
    ASSERT_EQ(config.decode_queue_size, 2);
    ASSERT_EQ(config.infer_queue_size, 18);
    ASSERT_EQ(config.log_level, std::string("info"));
}

// 2. ServerConfig JSON 往返 (serialize → deserialize)
TEST(server_config_json_roundtrip) {
    ServerConfig original;
    original.http_port = 9090;
    original.zmq_endpoint = "ipc:///tmp/test.ipc";
    original.num_infer_workers = 2;
    original.log_level = "debug";

    nlohmann::json j = original;
    std::cout << "    JSON: " << j.dump(2) << std::endl;

    auto restored = j.get<ServerConfig>();
    ASSERT_EQ(restored.http_port, 9090);
    ASSERT_EQ(restored.zmq_endpoint, std::string("ipc:///tmp/test.ipc"));
    ASSERT_EQ(restored.num_infer_workers, 2);
    ASSERT_EQ(restored.log_level, std::string("debug"));
    // 未修改的字段应保持默认值
    ASSERT_EQ(restored.decode_queue_size, 2);
}

// 3. StreamConfig + ModelConfig JSON 往返
TEST(stream_config_json_roundtrip) {
    StreamConfig stream;
    stream.cam_id = "cam_01";
    stream.rtsp_url = "rtsp://admin:pass@192.168.1.100:554/stream";
    stream.frame_skip = 5;

    ModelConfig model1;
    model1.model_path = "/weights/yolo_phone.rknn";
    model1.task_name = "phone_detection";
    model1.model_type = "yolov5";
    model1.input_width = 640;
    model1.input_height = 640;
    model1.conf_threshold = 0.3f;
    model1.labels_file = "/weights/phone_labels.txt";
    stream.models.push_back(model1);

    ModelConfig model2;
    model2.model_path = "/weights/yolo_smoking.rknn";
    model2.task_name = "smoking_detection";
    model2.model_type = "yolov8";
    model2.input_width = 640;
    model2.input_height = 640;
    stream.models.push_back(model2);

    nlohmann::json j = stream;
    std::cout << "    JSON: " << j.dump(2) << std::endl;

    auto restored = j.get<StreamConfig>();
    ASSERT_EQ(restored.cam_id, std::string("cam_01"));
    ASSERT_EQ(restored.frame_skip, 5);
    ASSERT_EQ(restored.models.size(), 2u);
    ASSERT_EQ(restored.models[0].task_name, std::string("phone_detection"));
    ASSERT_EQ(restored.models[0].model_type, std::string("yolov5"));
    ASSERT_EQ(restored.models[1].task_name, std::string("smoking_detection"));
    ASSERT_EQ(restored.models[1].model_type, std::string("yolov8"));
    // 未设置的字段应使用默认值
    ASSERT_TRUE(restored.models[1].nms_threshold > 0.44f && restored.models[1].nms_threshold < 0.46f);
}

// 4. ModelConfig 部分字段 JSON 解析 (使用默认值)
TEST(model_config_partial_json) {
    // 只提供必要字段, 其余使用默认值
    nlohmann::json j = {
        {"model_path", "/weights/test.rknn"},
        {"task_name", "test_task"}
    };

    auto model = j.get<ModelConfig>();
    ASSERT_EQ(model.model_path, std::string("/weights/test.rknn"));
    ASSERT_EQ(model.task_name, std::string("test_task"));
    ASSERT_EQ(model.model_type, std::string("yolov5"));  // 默认值
    ASSERT_EQ(model.input_width, 640);                     // 默认值
    ASSERT_EQ(model.input_height, 640);                    // 默认值
    ASSERT_TRUE(model.conf_threshold > 0.24f && model.conf_threshold < 0.26f);
}

// 5. 配置文件保存与加载
TEST(config_file_save_load) {
    std::string path = g_test_dir + "/server.json";

    ServerConfig original;
    original.http_port = 7777;
    original.log_level = "debug";

    ConfigManager::save_server_config(path, original);

    // 验证文件存在
    ASSERT_TRUE(fs::exists(path));

    auto loaded = ConfigManager::load_server_config(path);
    ASSERT_EQ(loaded.http_port, 7777);
    ASSERT_EQ(loaded.log_level, std::string("debug"));
}

// 6. 流配置持久化与恢复
TEST(streams_persistence) {
    std::string path = g_test_dir + "/streams.json";

    std::vector<StreamConfig> streams;

    StreamConfig s1;
    s1.cam_id = "cam_01";
    s1.rtsp_url = "rtsp://192.168.1.1/stream1";
    s1.frame_skip = 3;
    ModelConfig m1;
    m1.model_path = "/w/m1.rknn";
    m1.task_name = "detect";
    s1.models.push_back(m1);
    streams.push_back(s1);

    StreamConfig s2;
    s2.cam_id = "cam_02";
    s2.rtsp_url = "rtsp://192.168.1.2/stream2";
    s2.frame_skip = 10;
    streams.push_back(s2);

    // 保存
    ConfigManager::save_streams(path, streams);
    ASSERT_TRUE(fs::exists(path));

    // 加载
    auto loaded = ConfigManager::load_streams(path);
    ASSERT_EQ(loaded.size(), 2u);
    ASSERT_EQ(loaded[0].cam_id, std::string("cam_01"));
    ASSERT_EQ(loaded[0].models.size(), 1u);
    ASSERT_EQ(loaded[1].cam_id, std::string("cam_02"));
    ASSERT_EQ(loaded[1].frame_skip, 10);
}

// 7. FrameResult JSON 序列化
TEST(frame_result_json) {
    FrameResult result;
    result.cam_id = "cam_01";
    result.frame_id = 12345;
    result.timestamp_ms = 1677654321123;
    result.pts = 500000;
    result.original_width = 1920;
    result.original_height = 1080;

    ModelResult mr;
    mr.task_name = "phone_detection";
    mr.model_path = "/weights/phone.rknn";
    mr.inference_time_ms = 35.2;

    Detection det;
    det.class_id = 0;
    det.class_name = "phone";
    det.confidence = 0.95f;
    det.bbox = {100.0f, 200.0f, 300.0f, 400.0f};
    mr.detections.push_back(det);

    result.results.push_back(mr);

    nlohmann::json j = result;
    std::string json_str = j.dump(2);
    std::cout << "    FrameResult JSON:" << std::endl;
    // 逐行打印 (带缩进)
    std::istringstream iss(json_str);
    std::string line;
    while (std::getline(iss, line)) {
        std::cout << "      " << line << std::endl;
    }

    // 反序列化验证
    auto restored = j.get<FrameResult>();
    ASSERT_EQ(restored.cam_id, std::string("cam_01"));
    ASSERT_EQ(restored.frame_id, 12345u);
    ASSERT_EQ(restored.results.size(), 1u);
    ASSERT_EQ(restored.results[0].detections.size(), 1u);
    ASSERT_EQ(restored.results[0].detections[0].class_name, std::string("phone"));
    ASSERT_TRUE(restored.results[0].detections[0].confidence > 0.94f);
}

// 8. 错误处理
TEST(error_handling) {
    // 加载不存在的文件应抛出异常
    ASSERT_THROWS(ConfigManager::load_server_config("/nonexistent/path/config.json"));
    ASSERT_THROWS(ConfigManager::load_streams("/nonexistent/path/streams.json"));
}

// 9. 嵌套目录自动创建
TEST(auto_create_directory) {
    std::string path = g_test_dir + "/deep/nested/dir/config.json";

    ServerConfig config;
    config.http_port = 1234;
    ConfigManager::save_server_config(path, config);

    ASSERT_TRUE(fs::exists(path));
    auto loaded = ConfigManager::load_server_config(path);
    ASSERT_EQ(loaded.http_port, 1234);
}

// 10. ApiResponse JSON 序列化
TEST(api_response_json) {
    ApiResponse resp;
    resp.code = 0;
    resp.message = "success";
    resp.data = nlohmann::json{{"added", {"cam_01", "cam_02"}}, {"failed", nlohmann::json::array()}};

    nlohmann::json j = resp;
    std::cout << "    ApiResponse: " << j.dump() << std::endl;

    auto restored = j.get<ApiResponse>();
    ASSERT_EQ(restored.code, 0);
    ASSERT_EQ(restored.message, std::string("success"));
    ASSERT_EQ(restored.data["added"].size(), 2u);
}

// ============================================================
// 测试运行器
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Config & Types Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    setup_test_dir();

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

    cleanup_test_dir();

    std::cout << "========================================" << std::endl;
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed"
              << " (total " << (g_pass + g_fail) << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    return g_fail > 0 ? 1 : 0;
}
