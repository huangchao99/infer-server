#include "infer_server/common/logger.h"
#include "infer_server/common/config.h"

#include <csignal>
#include <atomic>
#include <iostream>
#include <thread>

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
    LOG_INFO("  Decode queue:     {}", config.decode_queue_size);
    LOG_INFO("  Infer queue:      {}", config.infer_queue_size);
    LOG_INFO("  Streams save:     {}", config.streams_save_path);

    // ========================
    // 注册信号处理
    // ========================
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ========================
    // TODO Phase 2+: 初始化各组件
    // - ModelManager (模型加载/缓存)
    // - InferenceWorkers (NPU 推理线程池)
    // - ZmqPublisher (ZeroMQ 结果发布)
    // - StreamManager (流生命周期管理)
    // - RestServer (REST API)
    // ========================

    // ========================
    // 加载持久化的流配置 (重启恢复)
    // ========================
    try {
        auto streams = infer_server::ConfigManager::load_streams(config.streams_save_path);
        if (!streams.empty()) {
            LOG_INFO("Found {} persisted stream(s), will auto-start after pipeline init", streams.size());
            for (const auto& s : streams) {
                LOG_INFO("  - [{}] {} ({} model(s), skip={})",
                    s.cam_id, s.rtsp_url, s.models.size(), s.frame_skip);
            }
            // TODO Phase 4: Auto-start streams via StreamManager
        }
    } catch (const std::exception& e) {
        LOG_DEBUG("No persisted streams to restore: {}", e.what());
    }

    // ========================
    // 主循环
    // ========================
    LOG_INFO("Server started. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ========================
    // 清理
    // ========================
    LOG_INFO("Shutting down...");
    // TODO Phase 2+: Stop all components gracefully

    LOG_INFO("Server stopped.");
    infer_server::logger::shutdown();
    return 0;
}
