#pragma once

/**
 * @file stream_manager.h
 * @brief RTSP 流生命周期管理器
 *
 * StreamManager 是 Phase 4 的核心编排组件:
 * - 管理所有 RTSP 流的添加/删除/启停
 * - 每个流拥有独立的解码线程
 * - 解码线程内部完成: RTSP解码 -> 跳帧 -> RGA缩放 -> 推理提交 + 图片缓存
 * - 自动重连 (指数退避)
 * - 运行时统计 (原子计数器)
 * - 配置持久化 (重启恢复)
 */

#include "infer_server/common/config.h"
#include "infer_server/common/types.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>
#include <functional>

namespace infer_server {

// 前向声明 (避免在头文件中暴露硬件依赖)
class ImageCache;

#ifdef HAS_RKNN
class InferenceEngine;
#endif

class JpegEncoder;

/**
 * @brief 流管理器
 */
class StreamManager {
public:
    /**
     * @brief 构造流管理器
     * @param config 服务器全局配置
     * @param engine 推理引擎指针 (可为 nullptr 如果 RKNN 不可用)
     * @param cache  图片缓存指针 (可为 nullptr 如果 TurboJPEG 不可用)
     */
    StreamManager(const ServerConfig& config,
#ifdef HAS_RKNN
                  InferenceEngine* engine,
#endif
                  ImageCache* cache);

    ~StreamManager();

    // 禁止拷贝
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    // === 流 CRUD ===

    /**
     * @brief 添加流并自动启动
     * @param stream_config 流配置 (cam_id 必须唯一)
     * @return true 添加成功
     */
    bool add_stream(const StreamConfig& stream_config);

    /**
     * @brief 移除流 (先停止再删除)
     * @param cam_id 摄像头 ID
     * @return true 移除成功
     */
    bool remove_stream(const std::string& cam_id);

    /**
     * @brief 启动已添加但停止的流
     */
    bool start_stream(const std::string& cam_id);

    /**
     * @brief 停止运行中的流 (不删除配置)
     */
    bool stop_stream(const std::string& cam_id);

    // === 批量操作 ===

    void start_all();
    void stop_all();

    // === 查询 ===

    /// 获取所有流的运行状态
    std::vector<StreamStatus> get_all_status() const;

    /// 获取单个流的运行状态
    std::optional<StreamStatus> get_status(const std::string& cam_id) const;

    /// 获取所有流的配置
    std::vector<StreamConfig> get_all_configs() const;

    /// 检查流是否存在
    bool has_stream(const std::string& cam_id) const;

    /// 当前流数量
    size_t stream_count() const;

    // === 持久化 ===

    /// 保存所有流配置到磁盘
    void save_configs() const;

    /// 从配置列表加载并启动所有流
    void load_and_start(const std::vector<StreamConfig>& configs);

    // === 生命周期 ===

    /// 优雅关闭 (停止所有流)
    void shutdown();

    /// 当推理结果完成时的回调 (用于递增 inferred_frames 统计)
    void on_infer_result(const FrameResult& result);

private:
    /// 流上下文 (每个流的内部状态)
    struct StreamContext {
        StreamConfig config;
        std::atomic<int> state{static_cast<int>(StreamState::Stopped)};
        std::thread decode_thread;
        std::atomic<bool> running{false};
        std::atomic<bool> stop_requested{false};

        // 原子统计计数器
        std::atomic<uint64_t> decoded_frames{0};
        std::atomic<uint64_t> inferred_frames{0};
        std::atomic<uint32_t> reconnect_count{0};
        std::string last_error;
        mutable std::mutex error_mutex;
        std::chrono::steady_clock::time_point start_time;

        // 每个流拥有独立的 JPEG 编码器
        std::unique_ptr<JpegEncoder> jpeg_encoder;

        // 标签缓存: model_path -> labels
        std::unordered_map<std::string, std::vector<std::string>> labels_cache;

        void set_error(const std::string& err) {
            std::lock_guard<std::mutex> lock(error_mutex);
            last_error = err;
        }
        std::string get_error() const {
            std::lock_guard<std::mutex> lock(error_mutex);
            return last_error;
        }
    };

    /// 解码线程主函数
    void decode_thread_func(StreamContext* ctx);

    /// 构造 StreamStatus 快照
    StreamStatus build_status(const StreamContext& ctx) const;

    /// 加载标签文件
    static std::vector<std::string> load_labels_file(const std::string& path);

    /// 停止流内部实现 (调用者需持有 mutex_)
    void stop_stream_internal(StreamContext& ctx);

    ServerConfig config_;
#ifdef HAS_RKNN
    InferenceEngine* engine_ = nullptr;
#endif
    ImageCache* cache_ = nullptr;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<StreamContext>> streams_;
};

} // namespace infer_server
