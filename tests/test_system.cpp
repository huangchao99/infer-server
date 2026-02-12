/**
 * @file test_system.cpp
 * @brief Phase 4 完整系统集成测试
 *
 * 需要全部硬件 (RKNN + FFmpeg + RGA + TurboJPEG + ZMQ) 和:
 * - 可访问的 RTSP 摄像头
 * - RKNN 模型文件 (.rknn)
 *
 * 测试内容:
 * 1. 启动完整服务器 (InferenceEngine + StreamManager + RestServer)
 * 2. 通过 HTTP API 添加 RTSP 流
 * 3. 验证解码、推理、ZMQ 输出、图片缓存
 * 4. 通过 HTTP API 查询流状态和服务器状态
 * 5. 获取缓存图片
 * 6. 测试流停止/启动/删除
 * 7. 测试持久化恢复
 *
 * 运行方式:
 *   sudo ./test_system <rtsp_url> <model.rknn> [model_type] [cam_id]
 *
 * 示例:
 *   sudo ./test_system \
 *     "rtsp://admin:pass@192.168.254.124:554/Streaming/Channels/102" \
 *     "/weight/yolo.rknn" \
 *     "yolov5" \
 *     "cam01"
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <fstream>

#ifdef HAS_HTTP

#include "infer_server/common/config.h"
#include "infer_server/common/logger.h"
#include "infer_server/stream/stream_manager.h"
#include "infer_server/api/rest_server.h"

#ifdef HAS_TURBOJPEG
#include "infer_server/cache/image_cache.h"
#endif

#ifdef HAS_RKNN
#include "infer_server/inference/inference_engine.h"
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static int g_pass = 0, g_fail = 0;
#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << #expr << std::endl; \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

static const int TEST_PORT = 18081;
static const std::string STREAMS_PATH = "/tmp/test_system_streams.json";

void print_usage(const char* prog) {
    std::cout << "Usage: sudo " << prog
              << " <rtsp_url> <model.rknn> [model_type] [cam_id]" << std::endl;
    std::cout << "  model_type: yolov5 (default), yolov8, yolov11" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string rtsp_url = argv[1];
    std::string model_path = argv[2];
    std::string model_type = (argc > 3) ? argv[3] : "yolov5";
    std::string cam_id = (argc > 4) ? argv[4] : "test_cam01";

    // 清理旧的持久化文件
    std::remove(STREAMS_PATH.c_str());

    infer_server::logger::init("info");

    std::cout << "=== System Integration Test ===" << std::endl;
    std::cout << "RTSP:  " << rtsp_url << std::endl;
    std::cout << "Model: " << model_path << std::endl;
    std::cout << "Type:  " << model_type << std::endl;
    std::cout << "CamID: " << cam_id << std::endl;

    // ---- 创建配置 ----
    infer_server::ServerConfig config;
    config.http_port = TEST_PORT;
    config.streams_save_path = STREAMS_PATH;
    config.num_infer_workers = 3;
    config.infer_queue_size = 12;
    config.cache_duration_sec = 5;
    config.cache_jpeg_quality = 75;
    config.cache_resize_width = 640;
    config.cache_max_memory_mb = 32;

    // ---- 创建组件 ----
#ifdef HAS_TURBOJPEG
    auto cache = std::make_unique<infer_server::ImageCache>(
        config.cache_duration_sec, config.cache_max_memory_mb);
    infer_server::ImageCache* cache_ptr = cache.get();
#else
    infer_server::ImageCache* cache_ptr = nullptr;
#endif

#ifdef HAS_RKNN
    auto engine = std::make_unique<infer_server::InferenceEngine>(config);
    if (!engine->init()) {
        std::cerr << "Failed to init InferenceEngine" << std::endl;
        return 1;
    }
    infer_server::InferenceEngine* engine_ptr = engine.get();
#else
    std::cerr << "RKNN not available, cannot run system test" << std::endl;
    return 1;
#endif

    auto stream_mgr = std::make_unique<infer_server::StreamManager>(
        config,
#ifdef HAS_RKNN
        engine_ptr,
#endif
        cache_ptr);

#ifdef HAS_RKNN
    engine->set_result_callback(
        [&stream_mgr](const infer_server::FrameResult& result) {
            stream_mgr->on_infer_result(result);
        });
#endif

    auto rest = std::make_unique<infer_server::RestServer>(
        *stream_mgr, cache_ptr,
#ifdef HAS_RKNN
        engine_ptr,
#endif
        config);

    if (!rest->start()) {
        std::cerr << "Failed to start REST server" << std::endl;
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    httplib::Client cli("localhost", TEST_PORT);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);

    // ============================================================
    // Test 1: 添加流 via REST API
    // ============================================================
    std::cout << "\n[Test 1] Add stream via POST /api/streams" << std::endl;
    {
        json body;
        body["cam_id"] = cam_id;
        body["rtsp_url"] = rtsp_url;
        body["frame_skip"] = 5;

        json model;
        model["model_path"] = model_path;
        model["task_name"] = "detection";
        model["model_type"] = model_type;
        model["input_width"] = 640;
        model["input_height"] = 640;
        model["conf_threshold"] = 0.25;
        model["nms_threshold"] = 0.45;
        body["models"] = json::array({model});

        auto res = cli.Post("/api/streams", body.dump(), "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
            auto j = json::parse(res->body);
            ASSERT_TRUE(j["code"].get<int>() == 0);
            std::cout << "  Response: " << j.dump() << std::endl;
        }
    }

    // ============================================================
    // Test 2: 等待几秒让解码和推理运行
    // ============================================================
    std::cout << "\n[Test 2] Waiting 8 seconds for decode & inference..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(8));

    // ============================================================
    // Test 3: 查询流状态
    // ============================================================
    std::cout << "\n[Test 3] GET /api/streams/" << cam_id << std::endl;
    {
        auto res = cli.Get(("/api/streams/" + cam_id).c_str());
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
            auto j = json::parse(res->body);
            auto data = j["data"];
            std::cout << "  Status:         " << data["status"].get<std::string>() << std::endl;
            std::cout << "  Decoded frames: " << data["decoded_frames"].get<uint64_t>() << std::endl;
            std::cout << "  Inferred frames:" << data["inferred_frames"].get<uint64_t>() << std::endl;
            std::cout << "  Decode FPS:     " << data["decode_fps"].get<double>() << std::endl;
            std::cout << "  Infer FPS:      " << data["infer_fps"].get<double>() << std::endl;
            std::cout << "  Reconnects:     " << data["reconnect_count"].get<uint32_t>() << std::endl;

            // 验证: 应该已经解码了一些帧
            ASSERT_TRUE(data["decoded_frames"].get<uint64_t>() > 0);
        }
    }

    // ============================================================
    // Test 4: 查询服务器全局状态
    // ============================================================
    std::cout << "\n[Test 4] GET /api/status" << std::endl;
    {
        auto res = cli.Get("/api/status");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
            auto j = json::parse(res->body);
            std::cout << "  Server status: " << j["data"].dump(2) << std::endl;
            ASSERT_TRUE(j["data"]["streams_total"].get<int>() == 1);
        }
    }

    // ============================================================
    // Test 5: 获取缓存图片
    // ============================================================
    std::cout << "\n[Test 5] GET /api/cache/image?stream_id=" << cam_id << std::endl;
    {
        auto res = cli.Get(("/api/cache/image?stream_id=" + cam_id).c_str());
        ASSERT_TRUE(res != nullptr);
        if (res) {
            if (res->status == 200) {
                std::cout << "  Got JPEG image: " << res->body.size() << " bytes" << std::endl;
                std::cout << "  Frame-Id:   " << res->get_header_value("X-Frame-Id") << std::endl;
                std::cout << "  Timestamp:  " << res->get_header_value("X-Timestamp-Ms") << std::endl;
                ASSERT_TRUE(res->body.size() > 100);  // JPEG应该至少大于100字节

                // 保存到文件方便查看
                std::ofstream out("/tmp/test_system_cached.jpg", std::ios::binary);
                out.write(res->body.data(), res->body.size());
                out.close();
                std::cout << "  Saved to /tmp/test_system_cached.jpg" << std::endl;
            } else {
                std::cout << "  No cached image (status=" << res->status << ")" << std::endl;
                // 在某些情况下可能没有缓存 (TurboJPEG/RGA 不可用)
            }
        }
    }

    // ============================================================
    // Test 6: 停止流
    // ============================================================
    std::cout << "\n[Test 6] POST /api/streams/" << cam_id << "/stop" << std::endl;
    {
        auto res = cli.Post(("/api/streams/" + cam_id + "/stop").c_str(), "", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 验证状态
    {
        auto res = cli.Get(("/api/streams/" + cam_id).c_str());
        if (res) {
            auto j = json::parse(res->body);
            std::cout << "  Status after stop: " << j["data"]["status"].get<std::string>() << std::endl;
            ASSERT_TRUE(j["data"]["status"].get<std::string>() == "stopped");
        }
    }

    // ============================================================
    // Test 7: 重新启动流
    // ============================================================
    std::cout << "\n[Test 7] POST /api/streams/" << cam_id << "/start" << std::endl;
    {
        auto res = cli.Post(("/api/streams/" + cam_id + "/start").c_str(), "", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 验证: 应该在运行
    {
        auto res = cli.Get(("/api/streams/" + cam_id).c_str());
        if (res) {
            auto j = json::parse(res->body);
            std::string status = j["data"]["status"].get<std::string>();
            std::cout << "  Status after restart: " << status << std::endl;
            ASSERT_TRUE(status == "running" || status == "starting" || status == "reconnecting");
        }
    }

    // ============================================================
    // Test 8: 验证持久化文件已存在
    // ============================================================
    std::cout << "\n[Test 8] Verify persistence file" << std::endl;
    {
        std::ifstream ifs(STREAMS_PATH);
        ASSERT_TRUE(ifs.is_open());
        if (ifs.is_open()) {
            auto j = json::parse(ifs);
            ASSERT_TRUE(j.contains("streams"));
            ASSERT_TRUE(j["streams"].size() == 1);
            ASSERT_TRUE(j["streams"][0]["cam_id"].get<std::string>() == cam_id);
            std::cout << "  Persistence file content: " << j.dump(2) << std::endl;
        }
    }

    // ============================================================
    // Test 9: 删除流
    // ============================================================
    std::cout << "\n[Test 9] DELETE /api/streams/" << cam_id << std::endl;
    {
        auto res = cli.Delete(("/api/streams/" + cam_id).c_str());
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 验证: 应该没有流了
    {
        auto res = cli.Get("/api/streams");
        if (res) {
            auto j = json::parse(res->body);
            ASSERT_TRUE(j["data"].size() == 0);
            std::cout << "  Streams after delete: " << j["data"].size() << std::endl;
        }
    }

    // ============================================================
    // Test 10: 模拟持久化恢复
    // ============================================================
    std::cout << "\n[Test 10] Simulate persistence recovery" << std::endl;
    {
        // 写入一个持久化文件
        json persist;
        json stream;
        stream["cam_id"] = "recovered_cam";
        stream["rtsp_url"] = rtsp_url;
        stream["frame_skip"] = 10;

        json model;
        model["model_path"] = model_path;
        model["task_name"] = "detection";
        model["model_type"] = model_type;
        model["input_width"] = 640;
        model["input_height"] = 640;
        stream["models"] = json::array({model});
        persist["streams"] = json::array({stream});

        std::ofstream ofs(STREAMS_PATH);
        ofs << persist.dump(2);
        ofs.close();

        // 加载并启动
        auto configs = infer_server::ConfigManager::load_streams(STREAMS_PATH);
        ASSERT_TRUE(configs.size() == 1);
        stream_mgr->load_and_start(configs);

        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 验证
        auto res = cli.Get("/api/streams/recovered_cam");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_TRUE(res->status == 200);
            auto j = json::parse(res->body);
            std::cout << "  Recovered stream status: " << j["data"]["status"].get<std::string>() << std::endl;
        }

        // 清理
        stream_mgr->remove_stream("recovered_cam");
    }

    // ============================================================
    // 清理
    // ============================================================
    std::cout << "\n--- Cleaning up ---" << std::endl;
    rest->stop();
    stream_mgr->shutdown();

#ifdef HAS_RKNN
    engine->shutdown();
#endif

    std::remove(STREAMS_PATH.c_str());

    // 结果
    std::cout << "\n=== System Integration Test Results ===" << std::endl;
    std::cout << "PASSED: " << g_pass << std::endl;
    std::cout << "FAILED: " << g_fail << std::endl;

    if (g_fail > 0) {
        std::cerr << "SOME TESTS FAILED!" << std::endl;
        return 1;
    }
    std::cout << "ALL TESTS PASSED!" << std::endl;

    infer_server::logger::shutdown();
    return 0;
}

#else // !HAS_HTTP

int main() {
    std::cout << "HTTP not enabled, cannot run system test" << std::endl;
    return 1;
}

#endif // HAS_HTTP
