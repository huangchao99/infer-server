#pragma once

/**
 * @file image_cache.h
 * @brief 每流图片缓存 (Ring Buffer)
 *
 * 为每个 RTSP 流维护一个 JPEG 帧的 Ring Buffer, 保留最近 N 秒的帧。
 * 用于报警层获取报警时刻的图片/视频截图。
 *
 * 特性:
 * - 每个流独立的 deque + mutex (不同流写入互不阻塞)
 * - 按时间自动淘汰过期帧
 * - 全局内存上限控制
 * - 支持精确时间戳查询和最近帧查询
 * - 线程安全
 */

#include "infer_server/common/types.h"
#include <deque>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
#include <optional>
#include <atomic>

namespace infer_server {

class ImageCache {
public:
    /// @param duration_sec    每流保留时长 (秒)
    /// @param max_memory_mb   全局最大缓存内存 (MB, 0=不限制)
    ImageCache(int duration_sec = 5, int max_memory_mb = 64);
    ~ImageCache() = default;

    // 禁止拷贝
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    /// 注册一个流 (可选, add_frame 会自动创建)
    void add_stream(const std::string& cam_id);

    /// 删除一个流及其所有缓存帧
    void remove_stream(const std::string& cam_id);

    /// 添加一帧到缓存
    /// 自动清理该流的过期帧; 如果全局内存超限, 淘汰最旧帧
    void add_frame(CachedFrame frame);

    /// 按精确时间戳获取帧
    /// @return 匹配的帧, 找不到返回 nullopt
    std::optional<CachedFrame> get_frame(
        const std::string& cam_id, int64_t timestamp_ms) const;

    /// 获取最接近指定时间戳的帧
    /// @return 最近的帧, 找不到返回 nullopt
    std::optional<CachedFrame> get_nearest_frame(
        const std::string& cam_id, int64_t timestamp_ms) const;

    /// 获取某流最新一帧
    std::optional<CachedFrame> get_latest_frame(
        const std::string& cam_id) const;

    /// 当前缓存的总内存使用 (字节)
    size_t total_memory_bytes() const;

    /// 当前缓存帧总数
    size_t total_frames() const;

    /// 某流当前缓存帧数
    size_t stream_frame_count(const std::string& cam_id) const;

    /// 已注册的流数量
    size_t stream_count() const;

private:
    /// 单个流的缓存
    struct StreamCache {
        std::deque<CachedFrame> frames;
        mutable std::mutex mutex;
        std::atomic<size_t> memory_bytes{0};  ///< 该流的 JPEG 总大小
    };

    /// 获取或创建流缓存
    std::shared_ptr<StreamCache> get_or_create_cache(const std::string& cam_id);

    /// 获取流缓存 (不创建)
    std::shared_ptr<StreamCache> get_cache(const std::string& cam_id) const;

    /// 清理单个流的过期帧
    void evict_expired(StreamCache& cache, int64_t now_ms);

    /// 全局内存淘汰
    void evict_global_memory();

    int duration_sec_;
    size_t max_memory_bytes_;

    mutable std::mutex map_mutex_;  ///< 保护 caches_ map
    std::unordered_map<std::string, std::shared_ptr<StreamCache>> caches_;

    std::atomic<size_t> total_memory_{0};  ///< 全局 JPEG 总大小
};

} // namespace infer_server
