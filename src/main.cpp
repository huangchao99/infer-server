/**
 * @file main.cpp
 * @brief Infer Server 主入口
 *
 * 完整生命周期:
 * 1. 加载配置 (ServerConfig)
 * 2. 初始化日志
 * 3. 创建 ImageCache
 * 4. 创建 InferenceEngine, init()
 * 5. 创建 StreamManager(config, engine, cache)
 * 6. 加载持久化流配置 -> StreamManager.load_and_start()
 * 7. 创建 RestServer(stream_mgr, cache, engine, config)
 * 8. RestServer.start()
 * 9. 主循环 (等待信号)
 * 10. RestServer.stop()
 * 11. StreamManager.shutdown()
 * 12. InferenceEngine.shutdown()
 * 13. 日志关闭
 */

#include "infer_server/common/logger.h"
#include "infer_server/common/config.h"
#include "infer_server/stream/stream_manager.h"

#ifdef HAS_RKNN
#include "infer_server/inference/inference_engine.h"
#endif

#ifdef HAS_TURBOJPEG
#include "infer_server/cache/image_cache.h"
#endif

#ifdef HAS_HTTP
#include "infer_server/api/rest_server.h"
#endif

#include <csignal>
#include <atomic>
#include <iostream>
#include <thread>
#include <memory>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
    LOG_INFO("Received signal {}, shutting down...", sig);
    g_running = false;
}

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [config_path]" << std::endl;
    std::cout << "  config_path: Path to server config JSON file (default: config/server.json)" << std::endl;
}

int main(int argc, char* argv[]) {
    // ========================
    // 解析命令行参数
    // ========================
    std::string config_path = "config/server.json";
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        config_path = arg;
    }

    // ========================
    // 加载配置
    // ========================
    infer_server::ServerConfig config;
    try {
        config = infer_server::ConfigManager::load_server_config(config_path);
        std::cout << "[startup] Loaded config from: " << config_path << std::endl;
    } catch (const std::exception& e) {
        std::cout << "[startup] Using default config (" << e.what() << ")" << std::endl;
    }

    // ========================
    // 初始化日志
    // ========================
    infer_server::logger::init(config.log_level);

    LOG_INFO("===================================");
    LOG_INFO("  Infer Server v0.1.0");
    LOG_INFO("===================================");
    LOG_INFO("Config:");
    LOG_INFO("  HTTP port:        {}", config.http_port);
    LOG_INFO("  ZMQ endpoint:     {}", config.zmq_endpoint);
    LOG_INFO("  Infer workers:    {}", config.num_infer_workers);
    LOG_INFO("  NPU cores:        {}", config.num_npu_cores);
    LOG_INFO("  Decode queue:     {}", config.decode_queue_size);
    LOG_INFO("  Infer queue:      {}", config.infer_queue_size);
    LOG_INFO("  Streams save:     {}", config.streams_save_path);
    LOG_INFO("  Cache duration:   {}s", config.cache_duration_sec);
    LOG_INFO("  Cache JPEG quality: {}", config.cache_jpeg_quality);
    LOG_INFO("  Cache max memory: {}MB", config.cache_max_memory_mb);

    // ========================
    // 注册信号处理
    // ========================
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ========================
    // 3. 创建 ImageCache
    // ========================
#ifdef HAS_TURBOJPEG
    auto image_cache = std::make_unique<infer_server::ImageCache>(
        config.cache_duration_sec, config.cache_max_memory_mb);
    LOG_INFO("ImageCache created (duration={}s, max_memory={}MB)",
             config.cache_duration_sec, config.cache_max_memory_mb);
    infer_server::ImageCache* cache_ptr = image_cache.get();
#else
    LOG_WARN("TurboJPEG not available, image cache disabled");
    infer_server::ImageCache* cache_ptr = nullptr;
#endif

    // ========================
    // 4. 创建 InferenceEngine
    // ========================
#ifdef HAS_RKNN
    auto inference_engine = std::make_unique<infer_server::InferenceEngine>(config);
    if (!inference_engine->init()) {
        LOG_ERROR("Failed to initialize InferenceEngine");
        return 1;
    }
    LOG_INFO("InferenceEngine initialized ({} workers)", inference_engine->worker_count());
    infer_server::InferenceEngine* engine_ptr = inference_engine.get();
#else
    LOG_WARN("RKNN not available, inference engine disabled");
#endif

    // ========================
    // 5. 创建 StreamManager
    // ========================
    auto stream_manager = std::make_unique<infer_server::StreamManager>(
        config,
#ifdef HAS_RKNN
        engine_ptr,
#endif
        cache_ptr);
    LOG_INFO("StreamManager created");

    // 注册推理结果回调 (用于统计 inferred_frames)
#ifdef HAS_RKNN
    inference_engine->set_result_callback(
        [&stream_manager](const infer_server::FrameResult& result) {
            stream_manager->on_infer_result(result);
        });
#endif

    // ========================
    // 6. 加载持久化的流配置 (重启恢复)
    // ========================
    try {
        auto streams = infer_server::ConfigManager::load_streams(config.streams_save_path);
        if (!streams.empty()) {
            LOG_INFO("Found {} persisted stream(s), auto-starting...", streams.size());
            for (const auto& s : streams) {
                LOG_INFO("  - [{}] {} ({} model(s), skip={})",
                    s.cam_id, s.rtsp_url, s.models.size(), s.frame_skip);
            }
            stream_manager->load_and_start(streams);
        }
    } catch (const std::exception& e) {
        LOG_DEBUG("No persisted streams to restore: {}", e.what());
    }

    // ========================
    // 7 & 8. 创建并启动 RestServer
    // ========================
#ifdef HAS_HTTP
    auto rest_server = std::make_unique<infer_server::RestServer>(
        *stream_manager,
        cache_ptr,
#ifdef HAS_RKNN
        engine_ptr,
#endif
        config);

    if (!rest_server->start()) {
        LOG_ERROR("Failed to start REST API server");
        // 不返回, 流管理仍可运行 (只是没有 HTTP 接口)
    }
#else
    LOG_WARN("HTTP not available, REST API disabled");
#endif

    // ========================
    // 9. 主循环
    // ========================
    LOG_INFO("Server started. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ========================
    // 10~13. 优雅关闭
    // ========================
    LOG_INFO("Shutting down...");

#ifdef HAS_HTTP
    if (rest_server) {
        rest_server->stop();
    }
#endif

    if (stream_manager) {
        stream_manager->shutdown();
    }

#ifdef HAS_RKNN
    if (inference_engine) {
        inference_engine->shutdown();
    }
#endif

    LOG_INFO("Server stopped.");
    infer_server::logger::shutdown();
    return 0;
}
