/**
 * @file test_rest_api.cpp
 * @brief REST API 单元测试
 *
 * 使用 httplib::Client 在本地测试所有 REST 端点。
 * 不依赖 RKNN/FFmpeg/RGA 硬件 (流添加后解码线程会因缺少
 * FFmpeg 而立即报错退出, 但 API 本身可正常验证)。
 *
 * 运行方式:
 *   ./test_rest_api
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>

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

// 简单测试框架
static int g_pass = 0, g_fail = 0;
#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL [" << __LINE__ << "]: " << #expr << std::endl; \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "FAIL [" << __LINE__ << "]: " << #a << " == " << #b \
                  << " (got " << _a << " vs " << _b << ")" << std::endl; \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

static const int TEST_PORT = 18080;

int main() {
    infer_server::logger::init("warn");

    // 配置
    infer_server::ServerConfig config;
    config.http_port = TEST_PORT;
    config.streams_save_path = "/tmp/test_rest_api_streams.json";

    // 创建组件
#ifdef HAS_TURBOJPEG
    auto cache = std::make_unique<infer_server::ImageCache>(5, 32);
    infer_server::ImageCache* cache_ptr = cache.get();
#else
    infer_server::ImageCache* cache_ptr = nullptr;
#endif

#ifdef HAS_RKNN
    auto engine = std::make_unique<infer_server::InferenceEngine>(config);
    engine->init();
    infer_server::InferenceEngine* engine_ptr = engine.get();
#endif

    auto stream_mgr = std::make_unique<infer_server::StreamManager>(
        config,
#ifdef HAS_RKNN
        engine_ptr,
#endif
        cache_ptr);

    auto rest = std::make_unique<infer_server::RestServer>(
        *stream_mgr, cache_ptr,
#ifdef HAS_RKNN
        engine_ptr,
#endif
        config);

    if (!rest->start()) {
        std::cerr << "Failed to start REST server for testing" << std::endl;
        return 1;
    }

    // 等待服务器就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    httplib::Client cli("localhost", TEST_PORT);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    std::cout << "=== REST API Unit Tests ===" << std::endl;

    // ----------------------------------------------------------
    // Test 1: GET /api/status
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 1] GET /api/status" << std::endl;
        auto res = cli.Get("/api/status");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(j["code"].get<int>(), 0);
            ASSERT_TRUE(j["data"].contains("version"));
            ASSERT_TRUE(j["data"].contains("uptime_seconds"));
            ASSERT_TRUE(j["data"].contains("streams_total"));
            ASSERT_EQ_INT(j["data"]["streams_total"].get<int>(), 0);
            std::cout << "  Response: " << j.dump(2) << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 2: GET /api/streams (empty)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 2] GET /api/streams (empty)" << std::endl;
        auto res = cli.Get("/api/streams");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(j["code"].get<int>(), 0);
            ASSERT_TRUE(j["data"].is_array());
            ASSERT_EQ_INT(static_cast<int>(j["data"].size()), 0);
        }
    }

    // ----------------------------------------------------------
    // Test 3: POST /api/streams (add stream)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 3] POST /api/streams (add cam01)" << std::endl;
        json body;
        body["cam_id"] = "cam01";
        body["rtsp_url"] = "rtsp://test:test@127.0.0.1:554/stream1";
        body["frame_skip"] = 5;
        body["models"] = json::array();
        // 不添加模型 - 纯测试 API 层

        auto res = cli.Post("/api/streams", body.dump(), "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(j["code"].get<int>(), 0);
            ASSERT_TRUE(j["data"]["cam_id"].get<std::string>() == "cam01");
            std::cout << "  Response: " << j.dump() << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 4: POST /api/streams (duplicate)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 4] POST /api/streams (duplicate cam01)" << std::endl;
        json body;
        body["cam_id"] = "cam01";
        body["rtsp_url"] = "rtsp://test:test@127.0.0.1:554/stream1";

        auto res = cli.Post("/api/streams", body.dump(), "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 409);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(j["code"].get<int>(), 409);
            std::cout << "  Response: " << j.dump() << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 5: POST /api/streams (missing cam_id)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 5] POST /api/streams (missing cam_id)" << std::endl;
        json body;
        body["rtsp_url"] = "rtsp://test:test@127.0.0.1:554/stream1";

        auto res = cli.Post("/api/streams", body.dump(), "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 400);
            std::cout << "  Status: " << res->status << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 6: POST /api/streams (invalid JSON)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 6] POST /api/streams (invalid JSON)" << std::endl;
        auto res = cli.Post("/api/streams", "not json {{{", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 400);
            std::cout << "  Status: " << res->status << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 7: POST /api/streams (add cam02)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 7] POST /api/streams (add cam02)" << std::endl;
        json body;
        body["cam_id"] = "cam02";
        body["rtsp_url"] = "rtsp://test:test@127.0.0.1:554/stream2";
        body["frame_skip"] = 3;

        auto res = cli.Post("/api/streams", body.dump(), "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
        }
    }

    // ----------------------------------------------------------
    // Test 8: GET /api/streams (should have 2)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 8] GET /api/streams (2 streams)" << std::endl;
        auto res = cli.Get("/api/streams");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(static_cast<int>(j["data"].size()), 2);
            std::cout << "  Streams: " << j["data"].size() << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 9: GET /api/streams/cam01 (single stream status)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 9] GET /api/streams/cam01" << std::endl;
        auto res = cli.Get("/api/streams/cam01");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_TRUE(j["data"]["cam_id"].get<std::string>() == "cam01");
            std::cout << "  Status: " << j["data"]["status"].get<std::string>() << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 10: GET /api/streams/nonexistent (404)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 10] GET /api/streams/nonexistent" << std::endl;
        auto res = cli.Get("/api/streams/nonexistent");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 404);
        }
    }

    // ----------------------------------------------------------
    // Test 11: POST /api/streams/cam01/stop
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 11] POST /api/streams/cam01/stop" << std::endl;
        auto res = cli.Post("/api/streams/cam01/stop", "", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            std::cout << "  Response: " << res->body << std::endl;
        }
    }

    // 等待线程停止
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ----------------------------------------------------------
    // Test 12: POST /api/streams/cam01/start
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 12] POST /api/streams/cam01/start" << std::endl;
        auto res = cli.Post("/api/streams/cam01/start", "", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
        }
    }

    // ----------------------------------------------------------
    // Test 13: POST /api/streams/stop_all
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 13] POST /api/streams/stop_all" << std::endl;
        auto res = cli.Post("/api/streams/stop_all", "", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ----------------------------------------------------------
    // Test 14: POST /api/streams/start_all
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 14] POST /api/streams/start_all" << std::endl;
        auto res = cli.Post("/api/streams/start_all", "", "application/json");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
        }
    }

    // ----------------------------------------------------------
    // Test 15: DELETE /api/streams/cam02
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 15] DELETE /api/streams/cam02" << std::endl;
        auto res = cli.Delete("/api/streams/cam02");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
        }
    }

    // ----------------------------------------------------------
    // Test 16: DELETE /api/streams/cam02 (already removed)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 16] DELETE /api/streams/cam02 (not found)" << std::endl;
        auto res = cli.Delete("/api/streams/cam02");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 404);
        }
    }

    // ----------------------------------------------------------
    // Test 17: GET /api/streams (should have 1)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 17] GET /api/streams (1 stream)" << std::endl;
        auto res = cli.Get("/api/streams");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(static_cast<int>(j["data"].size()), 1);
        }
    }

    // ----------------------------------------------------------
    // Test 18: GET /api/cache/image (no stream_id)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 18] GET /api/cache/image (no stream_id)" << std::endl;
        auto res = cli.Get("/api/cache/image");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            // 400 (missing param) or 503 (cache not available)
            ASSERT_TRUE(res->status == 400 || res->status == 503);
            std::cout << "  Status: " << res->status << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 19: GET /api/cache/image?stream_id=cam01 (no cached image)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 19] GET /api/cache/image?stream_id=cam01" << std::endl;
        auto res = cli.Get("/api/cache/image?stream_id=cam01");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            // 404 (no image) or 503 (cache not available)
            ASSERT_TRUE(res->status == 404 || res->status == 503);
            std::cout << "  Status: " << res->status << std::endl;
        }
    }

    // ----------------------------------------------------------
    // Test 20: GET /api/status (final check)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Test 20] GET /api/status (final)" << std::endl;
        auto res = cli.Get("/api/status");
        ASSERT_TRUE(res != nullptr);
        if (res) {
            ASSERT_EQ_INT(res->status, 200);
            auto j = json::parse(res->body);
            ASSERT_EQ_INT(j["data"]["streams_total"].get<int>(), 1);
            std::cout << "  Response: " << j["data"].dump(2) << std::endl;
        }
    }

    // 清理 cam01
    stream_mgr->remove_stream("cam01");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 停止服务器
    rest->stop();

    // 清理临时文件
    std::remove("/tmp/test_rest_api_streams.json");

#ifdef HAS_RKNN
    engine->shutdown();
#endif

    // 结果
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "PASSED: " << g_pass << std::endl;
    std::cout << "FAILED: " << g_fail << std::endl;

    if (g_fail > 0) {
        std::cerr << "SOME TESTS FAILED!" << std::endl;
        return 1;
    }
    std::cout << "ALL TESTS PASSED!" << std::endl;
    return 0;
}

#else // !HAS_HTTP

int main() {
    std::cout << "HTTP not enabled, skipping REST API tests" << std::endl;
    return 0;
}

#endif // HAS_HTTP
