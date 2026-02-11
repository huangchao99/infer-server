#pragma once

/**
 * @file types.h
 * @brief 推理服务器核心数据类型定义
 *
 * 所有需要 JSON 序列化的类型都使用 nlohmann/json 宏定义。
 * InferTask、DecodedFrame 等内部类型不需要 JSON 序列化。
 */

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <chrono>
#include <nlohmann/json.hpp>

namespace infer_server {

// ============================================================
// 配置类型 (来自用户请求, JSON 可序列化)
// ============================================================

/// 单个模型的配置
struct ModelConfig {
    std::string model_path;             ///< RKNN 模型文件路径
    std::string task_name;              ///< 任务名称标识 (如 "phone_detection")
    std::string model_type = "yolov5";  ///< 模型类型: "yolov5", "yolov8", "yolov11"
    int input_width  = 640;             ///< 模型输入宽度
    int input_height = 640;             ///< 模型输入高度
    float conf_threshold = 0.25f;       ///< 置信度阈值
    float nms_threshold  = 0.45f;       ///< NMS 阈值
    std::string labels_file;            ///< 类别标签文件路径 (可选, 每行一个类别名)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ModelConfig,
        model_path, task_name, model_type,
        input_width, input_height,
        conf_threshold, nms_threshold,
        labels_file
    )
};

/// 单个 RTSP 流的配置
struct StreamConfig {
    std::string cam_id;                 ///< 摄像头唯一标识
    std::string rtsp_url;               ///< RTSP 地址
    int frame_skip = 5;                 ///< 每 N 帧推理一次
    std::vector<ModelConfig> models;    ///< 使用的模型列表

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        StreamConfig,
        cam_id, rtsp_url, frame_skip, models
    )
};

// ============================================================
// 检测结果类型 (ZeroMQ 输出, JSON 可序列化)
// ============================================================

/// 检测框 (坐标相对于原始帧尺寸)
struct BBox {
    float x1 = 0.0f;   ///< 左上角 x
    float y1 = 0.0f;   ///< 左上角 y
    float x2 = 0.0f;   ///< 右下角 x
    float y2 = 0.0f;   ///< 右下角 y

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BBox, x1, y1, x2, y2)
};

/// 单个检测目标
struct Detection {
    int class_id = -1;              ///< 类别 ID
    std::string class_name;         ///< 类别名称 (来自 labels_file)
    float confidence = 0.0f;        ///< 置信度
    BBox bbox;                      ///< 检测框

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Detection,
        class_id, class_name, confidence, bbox
    )
};

/// 单个模型对单帧的推理结果
struct ModelResult {
    std::string task_name;              ///< 任务名称
    std::string model_path;             ///< 模型路径
    double inference_time_ms = 0.0;     ///< 推理耗时 (毫秒)
    std::vector<Detection> detections;  ///< 检测结果列表

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ModelResult,
        task_name, model_path, inference_time_ms, detections
    )
};

/// 单帧的完整推理结果 (所有模型聚合后)
struct FrameResult {
    std::string cam_id;             ///< 摄像头 ID
    std::string rtsp_url;           ///< RTSP 地址 (Phase 2 新增)
    uint64_t frame_id = 0;         ///< 帧序号 (全局递增)
    int64_t timestamp_ms = 0;      ///< 时间戳 (毫秒)
    int64_t pts = 0;               ///< 原始 PTS
    int original_width = 0;        ///< 原始帧宽度
    int original_height = 0;       ///< 原始帧高度
    std::vector<ModelResult> results;  ///< 各模型推理结果

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        FrameResult,
        cam_id, rtsp_url, frame_id, timestamp_ms, pts,
        original_width, original_height, results
    )
};

// ============================================================
// 流状态类型
// ============================================================

/// 流运行状态枚举
enum class StreamState {
    Stopped,
    Starting,
    Running,
    Reconnecting,
    Error
};

/// 状态枚举转字符串
inline std::string stream_state_to_string(StreamState state) {
    switch (state) {
        case StreamState::Stopped:      return "stopped";
        case StreamState::Starting:     return "starting";
        case StreamState::Running:      return "running";
        case StreamState::Reconnecting: return "reconnecting";
        case StreamState::Error:        return "error";
        default:                        return "unknown";
    }
}

/// 流的运行状态信息 (用于 API 响应, JSON 可序列化)
struct StreamStatus {
    std::string cam_id;
    std::string rtsp_url;
    std::string status = "stopped";
    int frame_skip = 0;
    std::vector<ModelConfig> models;

    // 运行时统计
    uint64_t decoded_frames = 0;
    uint64_t inferred_frames = 0;
    uint64_t dropped_frames = 0;
    double decode_fps = 0.0;
    double infer_fps = 0.0;
    uint32_t reconnect_count = 0;
    std::string last_error;
    double uptime_seconds = 0.0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        StreamStatus,
        cam_id, rtsp_url, status, frame_skip, models,
        decoded_frames, inferred_frames, dropped_frames,
        decode_fps, infer_fps, reconnect_count,
        last_error, uptime_seconds
    )
};

// ============================================================
// 内部类型 (不需要 JSON 序列化)
// ============================================================

/// 解码后的帧 (NV12 格式, 从 AVFrame 拷贝出的连续内存)
struct DecodedFrame {
    std::string cam_id;
    std::string rtsp_url;
    uint64_t frame_id = 0;
    int64_t pts = 0;
    int64_t timestamp_ms = 0;     ///< 系统时间戳 (毫秒, epoch)
    int width = 0;                ///< 帧宽度
    int height = 0;               ///< 帧高度

    /// NV12 数据 (Y plane + UV interleaved, 连续存储)
    /// 布局: [Y: width*height bytes] [UV: width*(height/2) bytes]
    std::shared_ptr<std::vector<uint8_t>> nv12_data;
};

/// 图片缓存帧 (JPEG 压缩后)
struct CachedFrame {
    std::string cam_id;
    uint64_t frame_id = 0;
    int64_t timestamp_ms = 0;
    int width = 0;
    int height = 0;
    std::shared_ptr<std::vector<uint8_t>> jpeg_data;

    /// JPEG 数据大小 (字节)
    size_t jpeg_size() const {
        return jpeg_data ? jpeg_data->size() : 0;
    }
};

/// 推理任务 (有界队列中的元素)
struct InferTask {
    // 帧标识信息
    std::string cam_id;
    std::string rtsp_url;           ///< RTSP 地址 (Phase 2 新增)
    uint64_t frame_id = 0;
    int64_t pts = 0;
    int64_t timestamp_ms = 0;
    int original_width = 0;
    int original_height = 0;

    // 模型信息
    std::string model_path;
    std::string task_name;
    std::string model_type;
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;
    std::vector<std::string> labels;

    // 输入数据 (RGA resize 后的 RGB 数据)
    std::shared_ptr<std::vector<uint8_t>> input_data;
    int input_width = 0;
    int input_height = 0;

    /// 结果聚合器 (同一帧的多模型任务共享)
    /// 实际类型: std::shared_ptr<FrameResultCollector>
    /// 单模型场景可为 nullptr, InferWorker 会直接组装 FrameResult
    std::shared_ptr<void> aggregator;
};

// ============================================================
// API 响应通用结构
// ============================================================

/// 通用 API 响应
struct ApiResponse {
    int code = 0;                   ///< 状态码 (0=成功)
    std::string message = "success"; ///< 描述信息
    nlohmann::json data;            ///< 返回数据

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ApiResponse,
        code, message, data
    )
};

} // namespace infer_server
