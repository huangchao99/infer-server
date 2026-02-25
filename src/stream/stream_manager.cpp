/**
 * @file stream_manager.cpp
 * @brief RTSP 流生命周期管理器实现
 */

#include "infer_server/stream/stream_manager.h"
#include "infer_server/common/logger.h"

#ifdef HAS_FFMPEG
#include "infer_server/decoder/hw_decoder.h"
#endif

#ifdef HAS_RGA
#include "infer_server/processor/rga_processor.h"
#endif

#ifdef HAS_RKNN
#include "infer_server/inference/inference_engine.h"
#include "infer_server/inference/frame_result_collector.h"
#endif

#ifdef HAS_TURBOJPEG
#include "infer_server/cache/jpeg_encoder.h"
#include "infer_server/cache/image_cache.h"
#endif

#include <fstream>
#include <algorithm>
#include <cmath>

namespace infer_server {

// ============================================================
// 构造/析构
// ============================================================

StreamManager::StreamManager(const ServerConfig& config,
#ifdef HAS_RKNN
                             InferenceEngine* engine,
#endif
                             ImageCache* cache)
    : config_(config)
#ifdef HAS_RKNN
    , engine_(engine)
#endif
    , cache_(cache)
{
}

StreamManager::~StreamManager() {
    shutdown();
}

// ============================================================
// 流 CRUD
// ============================================================

bool StreamManager::add_stream(const StreamConfig& stream_config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (stream_config.cam_id.empty()) {
            LOG_ERROR("Cannot add stream: cam_id is empty");
            return false;
        }

        if (streams_.count(stream_config.cam_id)) {
            LOG_WARN("Stream {} already exists", stream_config.cam_id);
            return false;
        }

        LOG_INFO("Adding stream: [{}] {} (skip={}, {} model(s))",
                 stream_config.cam_id, stream_config.rtsp_url,
                 stream_config.frame_skip, stream_config.models.size());

        auto ctx = std::make_unique<StreamContext>();
        ctx->config = stream_config;

#ifdef HAS_TURBOJPEG
        ctx->jpeg_encoder = std::make_unique<JpegEncoder>();
#endif

        // 预加载标签文件
        for (const auto& mc : stream_config.models) {
            if (!mc.labels_file.empty() && ctx->labels_cache.find(mc.model_path) == ctx->labels_cache.end()) {
                ctx->labels_cache[mc.model_path] = load_labels_file(mc.labels_file);
            }
        }

#ifdef HAS_RKNN
        // 预加载模型
        if (engine_) {
            engine_->load_models(stream_config.models);
        }
#endif

#ifdef HAS_TURBOJPEG
        // 注册到图片缓存
        if (cache_) {
            cache_->add_stream(stream_config.cam_id);
        }
#endif

        // 启动解码线程
        auto* ctx_ptr = ctx.get();
        ctx->stop_requested = false;
        ctx->running = true;
        ctx->state = static_cast<int>(StreamState::Starting);
        ctx->start_time = std::chrono::steady_clock::now();
        ctx->decode_thread = std::thread(&StreamManager::decode_thread_func, this, ctx_ptr);

        streams_[stream_config.cam_id] = std::move(ctx);
    }

    // 持久化 (在锁外调用, 避免死锁)
    save_configs();

    return true;
}

bool StreamManager::remove_stream(const std::string& cam_id) {
    std::unique_ptr<StreamContext> ctx_to_destroy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(cam_id);
        if (it == streams_.end()) {
            LOG_WARN("Cannot remove stream {}: not found", cam_id);
            return false;
        }

        LOG_INFO("Removing stream: [{}]", cam_id);
        stop_stream_internal(*it->second);

        ctx_to_destroy = std::move(it->second);
        streams_.erase(it);
    }

    // 在锁外等待线程结束并销毁
    if (ctx_to_destroy && ctx_to_destroy->decode_thread.joinable()) {
        ctx_to_destroy->decode_thread.join();
    }

#ifdef HAS_TURBOJPEG
    if (cache_) {
        cache_->remove_stream(cam_id);
    }
#endif

    // 持久化
    save_configs();

    return true;
}

bool StreamManager::start_stream(const std::string& cam_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(cam_id);
    if (it == streams_.end()) {
        LOG_WARN("Cannot start stream {}: not found", cam_id);
        return false;
    }

    auto& ctx = *it->second;
    if (ctx.running.load()) {
        LOG_WARN("Stream {} is already running", cam_id);
        return true;
    }

    LOG_INFO("Starting stream: [{}]", cam_id);

    // 等待旧线程结束
    if (ctx.decode_thread.joinable()) {
        ctx.decode_thread.join();
    }

    // 重置统计
    ctx.decoded_frames = 0;
    ctx.inferred_frames = 0;
    ctx.reconnect_count = 0;
    ctx.set_error("");

    ctx.stop_requested = false;
    ctx.running = true;
    ctx.state = static_cast<int>(StreamState::Starting);
    ctx.start_time = std::chrono::steady_clock::now();
    ctx.decode_thread = std::thread(&StreamManager::decode_thread_func, this, &ctx);

    return true;
}

bool StreamManager::stop_stream(const std::string& cam_id) {
    StreamContext* ctx_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(cam_id);
        if (it == streams_.end()) {
            LOG_WARN("Cannot stop stream {}: not found", cam_id);
            return false;
        }
        ctx_ptr = it->second.get();
        stop_stream_internal(*ctx_ptr);
    }

    // 在锁外等待线程结束
    if (ctx_ptr && ctx_ptr->decode_thread.joinable()) {
        ctx_ptr->decode_thread.join();
    }

    return true;
}

void StreamManager::stop_stream_internal(StreamContext& ctx) {
    if (!ctx.running.load()) return;
    LOG_INFO("Stopping stream: [{}]", ctx.config.cam_id);
    ctx.stop_requested = true;
}

// ============================================================
// 批量操作
// ============================================================

void StreamManager::start_all() {
    std::vector<std::string> cam_ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, ctx] : streams_) {
            if (!ctx->running.load()) {
                cam_ids.push_back(id);
            }
        }
    }
    for (auto& id : cam_ids) {
        start_stream(id);
    }
}

void StreamManager::stop_all() {
    std::vector<StreamContext*> contexts;
    {
        // 先发送停止信号
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, ctx] : streams_) {
            stop_stream_internal(*ctx);
            contexts.push_back(ctx.get());
        }
    }
    // 等待所有线程结束 (锁外, 避免死锁)
    for (auto* ctx : contexts) {
        if (ctx->decode_thread.joinable()) {
            ctx->decode_thread.join();
        }
    }
}

// ============================================================
// 查询
// ============================================================

std::vector<StreamStatus> StreamManager::get_all_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamStatus> result;
    result.reserve(streams_.size());
    for (const auto& [id, ctx] : streams_) {
        result.push_back(build_status(*ctx));
    }
    return result;
}

std::optional<StreamStatus> StreamManager::get_status(const std::string& cam_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(cam_id);
    if (it == streams_.end()) return std::nullopt;
    return build_status(*it->second);
}

std::vector<StreamConfig> StreamManager::get_all_configs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamConfig> result;
    result.reserve(streams_.size());
    for (const auto& [id, ctx] : streams_) {
        result.push_back(ctx->config);
    }
    return result;
}

bool StreamManager::has_stream(const std::string& cam_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.count(cam_id) > 0;
}

size_t StreamManager::stream_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.size();
}

StreamStatus StreamManager::build_status(const StreamContext& ctx) const {
    StreamStatus s;
    s.cam_id = ctx.config.cam_id;
    s.rtsp_url = ctx.config.rtsp_url;
    s.status = stream_state_to_string(static_cast<StreamState>(ctx.state.load()));
    s.frame_skip = ctx.config.frame_skip;
    s.models = ctx.config.models;
    s.decoded_frames = ctx.decoded_frames.load();
    s.inferred_frames = ctx.inferred_frames.load();
    s.reconnect_count = ctx.reconnect_count.load();
    s.last_error = ctx.get_error();

    auto now = std::chrono::steady_clock::now();
    s.uptime_seconds = std::chrono::duration<double>(now - ctx.start_time).count();

    if (s.uptime_seconds > 0.0) {
        s.decode_fps = static_cast<double>(s.decoded_frames) / s.uptime_seconds;
        s.infer_fps = static_cast<double>(s.inferred_frames) / s.uptime_seconds;
    }

    // dropped_frames: 由全局推理队列统计, 这里近似为 0
    s.dropped_frames = 0;

    return s;
}

// ============================================================
// 持久化
// ============================================================

void StreamManager::save_configs() const {
    try {
        // 使用 get_all_configs() 获取带锁的快照
        auto configs = get_all_configs();
        ConfigManager::save_streams(config_.streams_save_path, configs);
        LOG_DEBUG("Saved {} stream config(s) to {}", configs.size(), config_.streams_save_path);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save stream configs: {}", e.what());
    }
}

void StreamManager::load_and_start(const std::vector<StreamConfig>& configs) {
    LOG_INFO("Loading {} persisted stream(s)...", configs.size());
    for (const auto& c : configs) {
        if (!add_stream(c)) {
            LOG_ERROR("Failed to add persisted stream: [{}]", c.cam_id);
        }
    }
}

// ============================================================
// 回调
// ============================================================

void StreamManager::on_infer_result(const FrameResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(result.cam_id);
    if (it != streams_.end()) {
        it->second->inferred_frames.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================================================
// 关闭
// ============================================================

void StreamManager::shutdown() {
    LOG_INFO("StreamManager shutting down...");
    stop_all();
    LOG_INFO("StreamManager shutdown complete");
}

// ============================================================
// 工具函数
// ============================================================

std::vector<std::string> StreamManager::load_labels_file(const std::string& path) {
    std::vector<std::string> labels;
    if (path.empty()) return labels;

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Cannot open labels file: {}", path);
        return labels;
    }

    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            labels.push_back(line);
        }
    }

    LOG_DEBUG("Loaded {} labels from {}", labels.size(), path);
    return labels;
}

// ============================================================
// 解码线程主函数
// ============================================================

void StreamManager::decode_thread_func(StreamContext* ctx) {
    const std::string& cam_id = ctx->config.cam_id;
    LOG_INFO("[{}] Decode thread started", cam_id);

#ifndef HAS_FFMPEG
    LOG_ERROR("[{}] FFmpeg not available, cannot decode RTSP", cam_id);
    ctx->set_error("FFmpeg not available");
    ctx->state = static_cast<int>(StreamState::Error);
    ctx->running = false;
    return;
#else

    uint64_t local_frame_count = 0;
    int backoff_sec = 1;
    const int max_backoff_sec = 8;

    while (!ctx->stop_requested.load(std::memory_order_relaxed)) {
        // === 打开解码器 ===
        ctx->state = static_cast<int>(StreamState::Starting);
        HwDecoder decoder;
        HwDecoder::Config dec_cfg;
        dec_cfg.rtsp_url = ctx->config.rtsp_url;
        dec_cfg.tcp_transport = true;
        dec_cfg.connect_timeout_sec = 5;
        dec_cfg.read_timeout_sec = 5;

        LOG_INFO("[{}] Opening RTSP stream: {}", cam_id, ctx->config.rtsp_url);
        if (!decoder.open(dec_cfg)) {
            ctx->set_error("Failed to open RTSP stream");
            ctx->state = static_cast<int>(StreamState::Reconnecting);
            ctx->reconnect_count.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[{}] Failed to open, retrying in {}s...", cam_id, backoff_sec);

            // 指数退避等待
            for (int i = 0; i < backoff_sec * 10 && !ctx->stop_requested.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoff_sec = std::min(backoff_sec * 2, max_backoff_sec);
            continue;
        }

        // 打开成功, 重置退避
        backoff_sec = 1;
        ctx->state = static_cast<int>(StreamState::Running);
        ctx->set_error("");
        LOG_INFO("[{}] Stream opened: {}x{} @ {:.1f}fps codec={} hw={}",
                 cam_id, decoder.get_width(), decoder.get_height(),
                 decoder.get_fps(), decoder.get_codec_name(),
                 decoder.is_hardware() ? "yes" : "no");

        int orig_w = decoder.get_width();
        int orig_h = decoder.get_height();

        // === 解码循环 ===
        int skip = ctx->config.frame_skip;

        while (!ctx->stop_requested.load(std::memory_order_relaxed)) {
            local_frame_count++;
            bool need_process = (skip <= 1) ||
                (local_frame_count % static_cast<uint64_t>(skip)) == 0;

            // 跳帧时用轻量级路径：只推进解码器，不做 GPU→CPU 拷贝和 NV12 提取
            if (!need_process) {
                if (!decoder.skip_frame()) {
                    ctx->set_error("Decode failed or stream ended");
                    ctx->state = static_cast<int>(StreamState::Reconnecting);
                    ctx->reconnect_count.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN("[{}] Decode failed, reconnecting in {}s...", cam_id, backoff_sec);
                    decoder.close();
                    break;
                }
                ctx->decoded_frames.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            auto frame = decoder.decode_frame();
            if (!frame) {
                ctx->set_error("Decode failed or stream ended");
                ctx->state = static_cast<int>(StreamState::Reconnecting);
                ctx->reconnect_count.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("[{}] Decode failed, reconnecting in {}s...", cam_id, backoff_sec);
                decoder.close();
                break;
            }
            ctx->decoded_frames.fetch_add(1, std::memory_order_relaxed);

            // === 推理提交 ===
#if defined(HAS_RKNN) && defined(HAS_RGA)
            if (engine_ && !ctx->config.models.empty()) {
                int num_models = static_cast<int>(ctx->config.models.size());

                // 构造基础 FrameResult 用于 Collector
                FrameResult base_result;
                base_result.cam_id = cam_id;
                base_result.rtsp_url = ctx->config.rtsp_url;
                base_result.frame_id = frame->frame_id;
                base_result.timestamp_ms = frame->timestamp_ms;
                base_result.pts = frame->pts;
                base_result.original_width = orig_w;
                base_result.original_height = orig_h;

                // 多模型: 创建共享的 Collector
                std::shared_ptr<FrameResultCollector> collector;
                if (num_models > 1) {
                    collector = std::make_shared<FrameResultCollector>(num_models, base_result);
                }

                for (const auto& mc : ctx->config.models) {
                    // RGA: NV12 -> RGB (模型输入尺寸)
                    auto rgb_data = RgaProcessor::nv12_to_rgb_resize(
                        frame->nv12_data->data(), orig_w, orig_h,
                        mc.input_width, mc.input_height);

                    if (!rgb_data || rgb_data->empty()) {
                        LOG_WARN("[{}] RGA resize failed for model {}", cam_id, mc.task_name);
                        continue;
                    }

                    InferTask task;
                    task.cam_id = cam_id;
                    task.rtsp_url = ctx->config.rtsp_url;
                    task.frame_id = frame->frame_id;
                    task.pts = frame->pts;
                    task.timestamp_ms = frame->timestamp_ms;
                    task.original_width = orig_w;
                    task.original_height = orig_h;
                    task.model_path = mc.model_path;
                    task.task_name = mc.task_name;
                    task.model_type = mc.model_type;
                    task.conf_threshold = mc.conf_threshold;
                    task.nms_threshold = mc.nms_threshold;
                    task.input_data = rgb_data;
                    task.input_width = mc.input_width;
                    task.input_height = mc.input_height;

                    // 标签
                    auto lab_it = ctx->labels_cache.find(mc.model_path);
                    if (lab_it != ctx->labels_cache.end()) {
                        task.labels = lab_it->second;
                    }

                    // 聚合器
                    if (collector) {
                        task.aggregator = collector;
                    }

                    engine_->submit(std::move(task));
                }
            }
#endif // HAS_RKNN && HAS_RGA

            // === 图片缓存 ===
#if defined(HAS_TURBOJPEG) && defined(HAS_RGA)
            if (cache_ && ctx->jpeg_encoder && ctx->jpeg_encoder->is_valid()) {
                int cache_w = config_.cache_resize_width > 0 ? config_.cache_resize_width : orig_w;
                int cache_h = config_.cache_resize_height > 0
                    ? config_.cache_resize_height
                    : RgaProcessor::calc_proportional_height(orig_w, orig_h, cache_w);

                auto cache_rgb = RgaProcessor::nv12_to_rgb_resize(
                    frame->nv12_data->data(), orig_w, orig_h,
                    cache_w, cache_h);

                if (cache_rgb && !cache_rgb->empty()) {
                    auto jpeg = ctx->jpeg_encoder->encode(
                        cache_rgb->data(), cache_w, cache_h,
                        config_.cache_jpeg_quality);

                    if (!jpeg.empty()) {
                        CachedFrame cf;
                        cf.cam_id = cam_id;
                        cf.frame_id = frame->frame_id;
                        cf.timestamp_ms = frame->timestamp_ms;
                        cf.width = cache_w;
                        cf.height = cache_h;
                        cf.jpeg_data = std::make_shared<std::vector<uint8_t>>(std::move(jpeg));
                        cache_->add_frame(std::move(cf));
                    }
                }
            }
#endif // HAS_TURBOJPEG && HAS_RGA
        } // end decode loop

        decoder.close();

        // 等待退避时间后重连
        if (!ctx->stop_requested.load()) {
            for (int i = 0; i < backoff_sec * 10 && !ctx->stop_requested.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoff_sec = std::min(backoff_sec * 2, max_backoff_sec);
        }
    } // end reconnect loop

    ctx->state = static_cast<int>(StreamState::Stopped);
    ctx->running = false;
    LOG_INFO("[{}] Decode thread stopped (decoded {} frames)", cam_id, ctx->decoded_frames.load());
#endif // HAS_FFMPEG
}

} // namespace infer_server
