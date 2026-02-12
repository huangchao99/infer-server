#pragma once

/**
 * @file rest_server.h
 * @brief REST API 服务器 (基于 cpp-httplib)
 *
 * 提供 HTTP REST 接口用于:
 * - 流的 CRUD 管理 (添加/删除/启停)
 * - 查询流状态和服务器全局状态
 * - 获取图片缓存
 *
 * 所有端点:
 *   POST   /api/streams                 添加流 (含自动启动)
 *   DELETE /api/streams/:cam_id         移除流
 *   GET    /api/streams                 获取所有流状态
 *   GET    /api/streams/:cam_id         获取单个流状态
 *   POST   /api/streams/:cam_id/start   启动流
 *   POST   /api/streams/:cam_id/stop    停止流
 *   POST   /api/streams/start_all       启动所有流
 *   POST   /api/streams/stop_all        停止所有流
 *   GET    /api/status                  服务器全局状态
 *   GET    /api/cache/image             获取缓存图片 (JPEG)
 */

#ifdef HAS_HTTP

#include "infer_server/common/config.h"
#include "infer_server/stream/stream_manager.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

// 前向声明 httplib
namespace httplib {
class Server;
}

namespace infer_server {

class ImageCache;

#ifdef HAS_RKNN
class InferenceEngine;
#endif

class RestServer {
public:
    /**
     * @brief 构造 REST 服务器
     * @param mgr    流管理器引用
     * @param cache  图片缓存指针 (可为 nullptr)
     * @param engine 推理引擎指针 (可为 nullptr)
     * @param config 服务器配置
     */
    RestServer(StreamManager& mgr,
               ImageCache* cache,
#ifdef HAS_RKNN
               InferenceEngine* engine,
#endif
               const ServerConfig& config);

    ~RestServer();

    // 禁止拷贝
    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    /**
     * @brief 在独立线程中启动 HTTP 服务器
     * @return true 启动成功
     */
    bool start();

    /// 停止 HTTP 服务器
    void stop();

    /// 服务器是否正在运行
    bool is_running() const { return running_.load(); }

private:
    /// 注册所有路由
    void setup_routes();

    /// JSON 成功响应
    static std::string json_ok(const std::string& message, const nlohmann::json& data = nullptr);

    /// JSON 错误响应
    static std::string json_error(int code, const std::string& message);

    StreamManager& stream_mgr_;
    ImageCache* cache_ = nullptr;
#ifdef HAS_RKNN
    InferenceEngine* engine_ = nullptr;
#endif
    ServerConfig config_;

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace infer_server

#endif // HAS_HTTP
