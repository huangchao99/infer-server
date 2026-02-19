#pragma once

/**
 * @file hw_decoder.h
 * @brief FFmpeg + RKMPP 硬件解码器
 *
 * 封装 FFmpeg 的 h264_rkmpp 硬件解码器，从 RTSP 流读取并解码视频帧。
 * 输出 NV12 格式的 DecodedFrame（连续内存，无 stride padding）。
 *
 * 使用方式:
 *   HwDecoder decoder;
 *   HwDecoder::Config cfg;
 *   cfg.rtsp_url = "rtsp://...";
 *   decoder.open(cfg);
 *   while (auto frame = decoder.decode_frame()) {
 *       // process frame->nv12_data
 *   }
 *   decoder.close();
 */

#ifdef HAS_FFMPEG

#include "infer_server/common/types.h"
#include <string>
#include <optional>
#include <cstdint>

// Forward declarations for FFmpeg types (avoid including heavy headers here)
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace infer_server {

class HwDecoder {
public:
    /// 解码器配置
    struct Config {
        std::string rtsp_url;
        int connect_timeout_sec = 5;    ///< RTSP 连接超时 (秒)
        int read_timeout_sec = 5;       ///< 读取超时 (秒)
        bool tcp_transport = true;      ///< 使用 TCP 传输 (更可靠)
    };

    HwDecoder() = default;
    ~HwDecoder();

    // 禁止拷贝
    HwDecoder(const HwDecoder&) = delete;
    HwDecoder& operator=(const HwDecoder&) = delete;

    /// 打开 RTSP 流并初始化硬件解码器
    /// @return true 成功, false 失败 (错误信息通过 LOG_ERROR 输出)
    bool open(const Config& config);

    /// 解码一帧并提取 NV12 数据
    /// 阻塞直到获取到下一帧或发生错误
    /// @return DecodedFrame (NV12 格式), 如果流结束或出错返回 nullopt
    std::optional<DecodedFrame> decode_frame();

    /// 解码一帧但丢弃数据（跳帧优化）
    /// 保持解码器状态推进，但跳过 GPU→CPU 拷贝和 NV12 提取，
    /// 避免每帧 ~1.4MB 的内存分配和 memcpy 开销。
    /// @return true 成功解码（数据已丢弃），false 流结束或出错
    bool skip_frame();

    /// 关闭解码器并释放所有资源
    void close();

    /// 流是否已打开
    bool is_open() const { return is_open_; }

    /// 视频宽度
    int get_width() const { return width_; }

    /// 视频高度
    int get_height() const { return height_; }

    /// 视频帧率
    double get_fps() const { return fps_; }

    /// 解码器名称 (如 "h264_rkmpp")
    const std::string& get_codec_name() const { return codec_name_; }

    /// 是否使用硬件解码器
    bool is_hardware() const { return is_hw_decoder_; }

private:
    /// 从 AVFrame 提取 NV12 数据到连续内存
    std::shared_ptr<std::vector<uint8_t>> extract_nv12(AVFrame* frame);

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* sw_frame_ = nullptr;   // 硬件帧→软件帧转换用
    AVPacket* packet_ = nullptr;
    int video_stream_idx_ = -1;

    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    std::string codec_name_;
    bool is_open_ = false;
    bool is_hw_decoder_ = false;
};

} // namespace infer_server

#endif // HAS_FFMPEG
