#pragma once

/**
 * @file config.h
 * @brief 服务器配置管理
 *
 * ServerConfig:  服务器全局配置 (端口、队列大小、日志级别、缓存参数等)
 * ConfigManager: 配置文件的加载/保存, 流配置的持久化
 */

#include "types.h"
#include <string>
#include <vector>

namespace infer_server {

/// 服务器全局配置
struct ServerConfig {
    int http_port = 8080;                                       ///< REST API 端口
    std::string zmq_endpoint = "ipc:///tmp/infer_server.ipc";   ///< ZeroMQ IPC 地址
    int num_infer_workers = 3;                                  ///< 推理线程数 (建议等于 NPU 核心数)
    int decode_queue_size = 2;                                  ///< 每路解码输出队列大小
    int infer_queue_size = 18;                                  ///< 全局推理任务队列大小
    std::string streams_save_path = "/etc/infer-server/streams.json";  ///< 流配置持久化路径
    std::string log_level = "info";                             ///< 日志级别

    // === 图片缓存配置 (Phase 2) ===
    int cache_duration_sec = 5;         ///< Ring Buffer 保留时长 (秒)
    int cache_jpeg_quality = 75;        ///< JPEG 压缩质量 (1-100)
    int cache_resize_width = 640;       ///< 缓存图片宽度 (0=保持原始宽度)
    int cache_resize_height = 0;        ///< 缓存图片高度 (0=按宽度等比例计算)
    int cache_max_memory_mb = 64;       ///< 缓存最大总内存 (MB)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ServerConfig,
        http_port, zmq_endpoint, num_infer_workers,
        decode_queue_size, infer_queue_size,
        streams_save_path, log_level,
        cache_duration_sec, cache_jpeg_quality,
        cache_resize_width, cache_resize_height,
        cache_max_memory_mb
    )
};

/// 配置管理器
class ConfigManager {
public:
    /// 从 JSON 文件加载服务器配置
    /// @throws std::runtime_error 文件不存在或解析失败
    static ServerConfig load_server_config(const std::string& path);

    /// 保存服务器配置到 JSON 文件
    /// @throws std::runtime_error 写入失败
    static void save_server_config(const std::string& path, const ServerConfig& config);

    /// 从 JSON 文件加载已持久化的流配置
    /// @throws std::runtime_error 文件不存在或解析失败
    static std::vector<StreamConfig> load_streams(const std::string& path);

    /// 保存流配置到 JSON 文件 (用于重启恢复)
    /// @throws std::runtime_error 写入失败
    static void save_streams(const std::string& path, const std::vector<StreamConfig>& streams);

private:
    /// 确保文件所在目录存在
    static void ensure_directory(const std::string& file_path);
};

} // namespace infer_server
