/**
 * @file rest_server.cpp
 * @brief REST API 服务器实现
 */

#ifdef HAS_HTTP

#include "infer_server/api/rest_server.h"
#include "infer_server/common/logger.h"

#ifdef HAS_TURBOJPEG
#include "infer_server/cache/image_cache.h"
#endif

#ifdef HAS_RKNN
#include "infer_server/inference/inference_engine.h"
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace infer_server {

using json = nlohmann::json;

// ============================================================
// 构造/析构
// ============================================================

RestServer::RestServer(StreamManager& mgr,
                       ImageCache* cache,
#ifdef HAS_RKNN
                       InferenceEngine* engine,
#endif
                       const ServerConfig& config)
    : stream_mgr_(mgr)
    , cache_(cache)
#ifdef HAS_RKNN
    , engine_(engine)
#endif
    , config_(config)
{
    server_ = std::make_unique<httplib::Server>();
}

RestServer::~RestServer() {
    stop();
}

// ============================================================
// 生命周期
// ============================================================

bool RestServer::start() {
    if (running_.load()) {
        LOG_WARN("RestServer already running");
        return true;
    }

    setup_routes();
    start_time_ = std::chrono::steady_clock::now();

    server_thread_ = std::thread([this]() {
        LOG_INFO("REST API server starting on 0.0.0.0:{}", config_.http_port);
        running_ = true;
        bool ok = server_->listen("0.0.0.0", config_.http_port);
        running_ = false;
        if (!ok) {
            LOG_ERROR("REST API server listen failed on port {}", config_.http_port);
        } else {
            LOG_INFO("REST API server stopped");
        }
    });

    // 等待服务器实际开始监听
    for (int i = 0; i < 50; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (server_->is_running()) {
            LOG_INFO("REST API server is ready on port {}", config_.http_port);
            return true;
        }
    }

    LOG_ERROR("REST API server failed to start within 5 seconds");
    return false;
}

void RestServer::stop() {
    if (server_ && server_->is_running()) {
        LOG_INFO("Stopping REST API server...");
        server_->stop();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    running_ = false;
}

// ============================================================
// JSON 工具
// ============================================================

std::string RestServer::json_ok(const std::string& message, const json& data) {
    ApiResponse resp;
    resp.code = 0;
    resp.message = message;
    resp.data = data.is_null() ? json::object() : data;
    json j = resp;
    return j.dump();
}

std::string RestServer::json_error(int code, const std::string& message) {
    ApiResponse resp;
    resp.code = code;
    resp.message = message;
    resp.data = json::object();
    json j = resp;
    return j.dump();
}

// ============================================================
// 路由注册
// ============================================================

void RestServer::setup_routes() {
    // ----------------------------------------------------------
    // POST /api/streams -- 添加流
    // ----------------------------------------------------------
    server_->Post("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        try {
            auto j = json::parse(req.body);
            auto stream_config = j.get<StreamConfig>();

            if (stream_config.cam_id.empty()) {
                res.status = 400;
                res.set_content(json_error(400, "cam_id is required"), "application/json");
                return;
            }
            if (stream_config.rtsp_url.empty()) {
                res.status = 400;
                res.set_content(json_error(400, "rtsp_url is required"), "application/json");
                return;
            }

            if (stream_mgr_.has_stream(stream_config.cam_id)) {
                res.status = 409;
                res.set_content(json_error(409, "Stream " + stream_config.cam_id + " already exists"),
                                "application/json");
                return;
            }

            bool ok = stream_mgr_.add_stream(stream_config);
            if (ok) {
                json data;
                data["cam_id"] = stream_config.cam_id;
                res.set_content(json_ok("Stream " + stream_config.cam_id + " added", data),
                                "application/json");
            } else {
                res.status = 500;
                res.set_content(json_error(500, "Failed to add stream " + stream_config.cam_id),
                                "application/json");
            }
        } catch (const json::exception& e) {
            res.status = 400;
            res.set_content(json_error(400, std::string("Invalid JSON: ") + e.what()),
                            "application/json");
        }
    });

    // ----------------------------------------------------------
    // DELETE /api/streams/:cam_id -- 移除流
    // ----------------------------------------------------------
    server_->Delete(R"(/api/streams/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        std::string cam_id = req.matches[1];

        bool ok = stream_mgr_.remove_stream(cam_id);
        if (ok) {
            json data;
            data["cam_id"] = cam_id;
            res.set_content(json_ok("Stream " + cam_id + " removed", data), "application/json");
        } else {
            res.status = 404;
            res.set_content(json_error(404, "Stream " + cam_id + " not found"), "application/json");
        }
    });

    // ----------------------------------------------------------
    // GET /api/streams -- 获取所有流状态
    // ----------------------------------------------------------
    server_->Get("/api/streams", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        auto statuses = stream_mgr_.get_all_status();
        json data = json::array();
        for (const auto& s : statuses) {
            data.push_back(s);
        }
        res.set_content(json_ok("success", data), "application/json");
    });

    // ----------------------------------------------------------
    // POST /api/streams/start_all -- 启动所有流
    // (必须在 :cam_id 路由之前注册以避免路由冲突)
    // ----------------------------------------------------------
    server_->Post("/api/streams/start_all", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        stream_mgr_.start_all();
        res.set_content(json_ok("All streams started"), "application/json");
    });

    // ----------------------------------------------------------
    // POST /api/streams/stop_all -- 停止所有流
    // ----------------------------------------------------------
    server_->Post("/api/streams/stop_all", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        stream_mgr_.stop_all();
        res.set_content(json_ok("All streams stopped"), "application/json");
    });

    // ----------------------------------------------------------
    // GET /api/streams/:cam_id -- 获取单个流状态
    // ----------------------------------------------------------
    server_->Get(R"(/api/streams/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        std::string cam_id = req.matches[1];

        auto status = stream_mgr_.get_status(cam_id);
        if (status) {
            json data = *status;
            res.set_content(json_ok("success", data), "application/json");
        } else {
            res.status = 404;
            res.set_content(json_error(404, "Stream " + cam_id + " not found"), "application/json");
        }
    });

    // ----------------------------------------------------------
    // POST /api/streams/:cam_id/start -- 启动单个流
    // ----------------------------------------------------------
    server_->Post(R"(/api/streams/([^/]+)/start)", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        std::string cam_id = req.matches[1];

        bool ok = stream_mgr_.start_stream(cam_id);
        if (ok) {
            json data;
            data["cam_id"] = cam_id;
            res.set_content(json_ok("Stream " + cam_id + " started", data), "application/json");
        } else {
            res.status = 404;
            res.set_content(json_error(404, "Stream " + cam_id + " not found or already running"),
                            "application/json");
        }
    });

    // ----------------------------------------------------------
    // POST /api/streams/:cam_id/stop -- 停止单个流
    // ----------------------------------------------------------
    server_->Post(R"(/api/streams/([^/]+)/stop)", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        std::string cam_id = req.matches[1];

        bool ok = stream_mgr_.stop_stream(cam_id);
        if (ok) {
            json data;
            data["cam_id"] = cam_id;
            res.set_content(json_ok("Stream " + cam_id + " stopped", data), "application/json");
        } else {
            res.status = 404;
            res.set_content(json_error(404, "Stream " + cam_id + " not found"),
                            "application/json");
        }
    });

    // ----------------------------------------------------------
    // GET /api/status -- 服务器全局状态
    // ----------------------------------------------------------
    server_->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");

        auto now = std::chrono::steady_clock::now();
        double uptime = std::chrono::duration<double>(now - start_time_).count();

        auto all_status = stream_mgr_.get_all_status();
        int running_count = 0;
        for (const auto& s : all_status) {
            if (s.status == "running") running_count++;
        }

        json data;
        data["version"] = "0.1.0";
        data["uptime_seconds"] = uptime;
        data["streams_total"] = static_cast<int>(all_status.size());
        data["streams_running"] = running_count;

#ifdef HAS_RKNN
        if (engine_) {
            data["infer_queue_size"] = engine_->queue_size();
            data["infer_queue_dropped"] = engine_->queue_dropped();
            data["infer_total_processed"] = engine_->total_processed();
#ifdef HAS_ZMQ
            data["zmq_published"] = engine_->zmq_published_count();
#endif
        }
#endif

#ifdef HAS_TURBOJPEG
        if (cache_) {
            double mem_mb = static_cast<double>(cache_->total_memory_bytes()) / (1024.0 * 1024.0);
            data["cache_memory_mb"] = std::round(mem_mb * 100.0) / 100.0;
            data["cache_total_frames"] = cache_->total_frames();
        }
#endif

        res.set_content(json_ok("success", data), "application/json");
    });

    // ----------------------------------------------------------
    // GET /api/cache/image -- 获取缓存图片
    // 参数: stream_id (必须), ts (可选, 毫秒时间戳), latest (可选, "true")
    // ----------------------------------------------------------
    server_->Get("/api/cache/image", [this](const httplib::Request& req, httplib::Response& res) {
#ifdef HAS_TURBOJPEG
        if (!cache_) {
            res.status = 503;
            res.set_header("Content-Type", "application/json");
            res.set_content(json_error(503, "Image cache not available"), "application/json");
            return;
        }

        std::string stream_id;
        if (req.has_param("stream_id")) {
            stream_id = req.get_param_value("stream_id");
        }
        if (stream_id.empty()) {
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.set_content(json_error(400, "stream_id parameter is required"), "application/json");
            return;
        }

        std::optional<CachedFrame> frame;

        // latest=true 或未指定 ts -> 返回最新帧
        bool want_latest = false;
        if (req.has_param("latest")) {
            want_latest = (req.get_param_value("latest") == "true");
        }

        if (want_latest || !req.has_param("ts")) {
            frame = cache_->get_latest_frame(stream_id);
        } else {
            int64_t ts = 0;
            try {
                ts = std::stoll(req.get_param_value("ts"));
            } catch (...) {
                res.status = 400;
                res.set_header("Content-Type", "application/json");
                res.set_content(json_error(400, "Invalid ts parameter"), "application/json");
                return;
            }
            frame = cache_->get_nearest_frame(stream_id, ts);
        }

        if (frame && frame->jpeg_data && !frame->jpeg_data->empty()) {
            res.set_header("Content-Type", "image/jpeg");
            res.set_header("X-Frame-Id", std::to_string(frame->frame_id));
            res.set_header("X-Timestamp-Ms", std::to_string(frame->timestamp_ms));
            res.set_header("X-Width", std::to_string(frame->width));
            res.set_header("X-Height", std::to_string(frame->height));
            res.set_content(
                std::string(reinterpret_cast<const char*>(frame->jpeg_data->data()),
                            frame->jpeg_data->size()),
                "image/jpeg");
        } else {
            res.status = 404;
            res.set_header("Content-Type", "application/json");
            res.set_content(json_error(404, "No cached image found for stream " + stream_id),
                            "application/json");
        }
#else
        (void)req;
        res.status = 503;
        res.set_header("Content-Type", "application/json");
        res.set_content(json_error(503, "Image cache not compiled (TurboJPEG unavailable)"),
                        "application/json");
#endif
    });

    LOG_DEBUG("All REST API routes registered");
}

} // namespace infer_server

#endif // HAS_HTTP
