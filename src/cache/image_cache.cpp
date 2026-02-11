#include "infer_server/cache/image_cache.h"
#include "infer_server/common/logger.h"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace infer_server {

ImageCache::ImageCache(int duration_sec, int max_memory_mb)
    : duration_sec_(duration_sec)
    , max_memory_bytes_(static_cast<size_t>(max_memory_mb) * 1024 * 1024)
{
    LOG_INFO("ImageCache created: duration={}s, max_memory={}MB",
             duration_sec_, max_memory_mb);
}

void ImageCache::add_stream(const std::string& cam_id) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    if (caches_.find(cam_id) == caches_.end()) {
        caches_[cam_id] = std::make_shared<StreamCache>();
        LOG_DEBUG("ImageCache: added stream {}", cam_id);
    }
}

void ImageCache::remove_stream(const std::string& cam_id) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = caches_.find(cam_id);
    if (it != caches_.end()) {
        // 减去该流的内存
        total_memory_ -= it->second->memory_bytes.load();
        caches_.erase(it);
        LOG_DEBUG("ImageCache: removed stream {}", cam_id);
    }
}

void ImageCache::add_frame(CachedFrame frame) {
    auto cache = get_or_create_cache(frame.cam_id);
    if (!cache) return;

    size_t frame_size = frame.jpeg_size();

    {
        std::lock_guard<std::mutex> lock(cache->mutex);

        // 清理该流的过期帧
        evict_expired(*cache, frame.timestamp_ms);

        // 添加新帧
        cache->frames.push_back(std::move(frame));
        cache->memory_bytes += frame_size;
    }

    total_memory_ += frame_size;

    // 全局内存检查
    if (max_memory_bytes_ > 0 && total_memory_.load() > max_memory_bytes_) {
        evict_global_memory();
    }
}

std::optional<CachedFrame> ImageCache::get_frame(
    const std::string& cam_id, int64_t timestamp_ms) const
{
    auto cache = get_cache(cam_id);
    if (!cache) return std::nullopt;

    std::lock_guard<std::mutex> lock(cache->mutex);
    for (const auto& f : cache->frames) {
        if (f.timestamp_ms == timestamp_ms) {
            return f;  // 返回拷贝 (shared_ptr 引用计数+1)
        }
    }
    return std::nullopt;
}

std::optional<CachedFrame> ImageCache::get_nearest_frame(
    const std::string& cam_id, int64_t timestamp_ms) const
{
    auto cache = get_cache(cam_id);
    if (!cache) return std::nullopt;

    std::lock_guard<std::mutex> lock(cache->mutex);
    if (cache->frames.empty()) return std::nullopt;

    // 二分查找最接近的帧 (frames 按 timestamp_ms 有序)
    const CachedFrame* best = nullptr;
    int64_t best_diff = INT64_MAX;

    for (const auto& f : cache->frames) {
        int64_t diff = std::abs(f.timestamp_ms - timestamp_ms);
        if (diff < best_diff) {
            best_diff = diff;
            best = &f;
        }
    }

    if (best) return *best;
    return std::nullopt;
}

std::optional<CachedFrame> ImageCache::get_latest_frame(
    const std::string& cam_id) const
{
    auto cache = get_cache(cam_id);
    if (!cache) return std::nullopt;

    std::lock_guard<std::mutex> lock(cache->mutex);
    if (cache->frames.empty()) return std::nullopt;

    return cache->frames.back();
}

size_t ImageCache::total_memory_bytes() const {
    return total_memory_.load();
}

size_t ImageCache::total_frames() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    size_t count = 0;
    for (const auto& [cam_id, cache] : caches_) {
        std::lock_guard<std::mutex> cache_lock(cache->mutex);
        count += cache->frames.size();
    }
    return count;
}

size_t ImageCache::stream_frame_count(const std::string& cam_id) const {
    auto cache = get_cache(cam_id);
    if (!cache) return 0;
    std::lock_guard<std::mutex> lock(cache->mutex);
    return cache->frames.size();
}

size_t ImageCache::stream_count() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    return caches_.size();
}

// ============================================================
// Private
// ============================================================

std::shared_ptr<ImageCache::StreamCache> ImageCache::get_or_create_cache(
    const std::string& cam_id)
{
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = caches_.find(cam_id);
    if (it != caches_.end()) {
        return it->second;
    }
    auto cache = std::make_shared<StreamCache>();
    caches_[cam_id] = cache;
    return cache;
}

std::shared_ptr<ImageCache::StreamCache> ImageCache::get_cache(
    const std::string& cam_id) const
{
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = caches_.find(cam_id);
    if (it != caches_.end()) {
        return it->second;
    }
    return nullptr;
}

void ImageCache::evict_expired(StreamCache& cache, int64_t now_ms) {
    // 调用方已持有 cache.mutex
    int64_t threshold = now_ms - static_cast<int64_t>(duration_sec_) * 1000;

    while (!cache.frames.empty() && cache.frames.front().timestamp_ms < threshold) {
        size_t frame_size = cache.frames.front().jpeg_size();
        cache.frames.pop_front();
        cache.memory_bytes -= frame_size;
        total_memory_ -= frame_size;
    }
}

void ImageCache::evict_global_memory() {
    // 从所有流中淘汰最旧的帧, 直到内存低于上限
    std::lock_guard<std::mutex> map_lock(map_mutex_);

    int evict_count = 0;
    while (total_memory_.load() > max_memory_bytes_) {
        // 找到最旧帧所在的流
        std::string oldest_cam;
        int64_t oldest_ts = INT64_MAX;

        for (const auto& [cam_id, cache] : caches_) {
            std::lock_guard<std::mutex> cache_lock(cache->mutex);
            if (!cache->frames.empty()) {
                int64_t ts = cache->frames.front().timestamp_ms;
                if (ts < oldest_ts) {
                    oldest_ts = ts;
                    oldest_cam = cam_id;
                }
            }
        }

        if (oldest_cam.empty()) break;  // 所有流都为空

        // 淘汰该帧
        auto& cache = caches_[oldest_cam];
        std::lock_guard<std::mutex> cache_lock(cache->mutex);
        if (!cache->frames.empty()) {
            size_t frame_size = cache->frames.front().jpeg_size();
            cache->frames.pop_front();
            cache->memory_bytes -= frame_size;
            total_memory_ -= frame_size;
            evict_count++;
        }
    }

    if (evict_count > 0) {
        LOG_DEBUG("ImageCache: evicted {} frames for memory limit ({:.1f}MB / {:.1f}MB)",
                  evict_count,
                  total_memory_.load() / (1024.0 * 1024.0),
                  max_memory_bytes_ / (1024.0 * 1024.0));
    }
}

} // namespace infer_server
