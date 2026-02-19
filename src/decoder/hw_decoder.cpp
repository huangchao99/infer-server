#ifdef HAS_FFMPEG

#include "infer_server/decoder/hw_decoder.h"
#include "infer_server/common/logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
}

#include <cstring>
#include <chrono>

namespace infer_server {

HwDecoder::~HwDecoder() {
    close();
}

bool HwDecoder::open(const Config& config) {
    if (is_open_) {
        LOG_WARN("Decoder already open, closing first");
        close();
    }

    LOG_INFO("Opening RTSP stream: {}", config.rtsp_url);

    // ========================
    // 设置 RTSP 选项
    // ========================
    AVDictionary* opts = nullptr;
    if (config.tcp_transport) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    }
    // 连接超时 (微秒)
    std::string timeout_us = std::to_string(
        static_cast<int64_t>(config.connect_timeout_sec) * 1000000);
    av_dict_set(&opts, "stimeout", timeout_us.c_str(), 0);
    // 最大分析时长
    av_dict_set(&opts, "analyzeduration", "2000000", 0);
    av_dict_set(&opts, "probesize", "2000000", 0);

    // ========================
    // 打开输入流
    // ========================
    int ret = avformat_open_input(&fmt_ctx_, config.rtsp_url.c_str(),
                                  nullptr, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to open RTSP stream: {} ({})", config.rtsp_url, errbuf);
        return false;
    }

    // ========================
    // 获取流信息
    // ========================
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to find stream info");
        close();
        return false;
    }

    // ========================
    // 查找视频流
    // ========================
    video_stream_idx_ = av_find_best_stream(
        fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        LOG_ERROR("No video stream found in RTSP source");
        close();
        return false;
    }

    AVStream* stream = fmt_ctx_->streams[video_stream_idx_];
    LOG_INFO("Video stream #{}: codec_id={}, {}x{}", video_stream_idx_,
             stream->codecpar->codec_id,
             stream->codecpar->width, stream->codecpar->height);

    // ========================
    // 查找解码器 (优先 RKMPP 硬件解码)
    // ========================
    const AVCodec* codec = nullptr;

    // 根据编码格式选择对应的 RKMPP 解码器
    if (stream->codecpar->codec_id == AV_CODEC_ID_H264) {
        codec = avcodec_find_decoder_by_name("h264_rkmpp");
    } else if (stream->codecpar->codec_id == AV_CODEC_ID_H265 ||
               stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        codec = avcodec_find_decoder_by_name("hevc_rkmpp");
    }

    if (codec) {
        is_hw_decoder_ = true;
        LOG_INFO("Using hardware decoder: {}", codec->name);
    } else {
        // 回退到软件解码
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            LOG_ERROR("No decoder found for codec_id {}", stream->codecpar->codec_id);
            close();
            return false;
        }
        is_hw_decoder_ = false;
        LOG_WARN("Hardware decoder not available, using software decoder: {}",
                 codec->name);
    }

    codec_name_ = codec->name;

    // ========================
    // 创建并配置解码器上下文
    // ========================
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        LOG_ERROR("Failed to allocate codec context");
        close();
        return false;
    }

    ret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (ret < 0) {
        LOG_ERROR("Failed to copy codec parameters to context");
        close();
        return false;
    }

    // 设置线程数 (软件解码时有效)
    if (!is_hw_decoder_) {
        codec_ctx_->thread_count = 2;
    }

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to open codec: {}", errbuf);
        close();
        return false;
    }

    // ========================
    // 获取视频参数
    // ========================
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // 计算帧率
    if (stream->avg_frame_rate.den > 0 && stream->avg_frame_rate.num > 0) {
        fps_ = av_q2d(stream->avg_frame_rate);
    } else if (stream->r_frame_rate.den > 0 && stream->r_frame_rate.num > 0) {
        fps_ = av_q2d(stream->r_frame_rate);
    } else {
        fps_ = 25.0;
        LOG_WARN("Could not determine FPS, defaulting to {:.1f}", fps_);
    }

    // ========================
    // 分配帧和数据包
    // ========================
    frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !sw_frame_ || !packet_) {
        LOG_ERROR("Failed to allocate frame/packet");
        close();
        return false;
    }

    is_open_ = true;
    LOG_INFO("Decoder opened successfully: {}x{} @ {:.1f}fps, codec={}, hw={}",
             width_, height_, fps_, codec_name_, is_hw_decoder_);

    return true;
}

std::optional<DecodedFrame> HwDecoder::decode_frame() {
    if (!is_open_) {
        return std::nullopt;
    }

    while (true) {
        // 读取一个数据包
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("Stream EOF");
            } else {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Error reading frame: {}", errbuf);
            }
            return std::nullopt;
        }

        // 跳过非视频包
        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        // 送入解码器
        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN)) {
                LOG_WARN("Error sending packet to decoder, skipping");
            }
            continue;
        }

        // 尝试获取解码后的帧
        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN)) {
            continue;  // 需要更多数据包
        }
        if (ret < 0) {
            LOG_ERROR("Error receiving frame from decoder");
            return std::nullopt;
        }

        // ========================
        // 获取 NV12 数据
        // ========================
        AVFrame* src_frame = frame_;
        bool transferred = false;

        // 如果是硬件帧 (DRM_PRIME 或有 hw_frames_ctx)，需要转换到 CPU 内存
        if (frame_->hw_frames_ctx != nullptr) {
            ret = av_hwframe_transfer_data(sw_frame_, frame_, 0);
            if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARN("Failed to transfer HW frame: {}, skipping", errbuf);
                av_frame_unref(frame_);
                continue;
            }
            sw_frame_->pts = frame_->pts;
            src_frame = sw_frame_;
            transferred = true;
        }

        // 验证像素格式
        if (src_frame->format != AV_PIX_FMT_NV12) {
            LOG_WARN("Unexpected pixel format: {} (expected NV12={}), skipping",
                     src_frame->format, AV_PIX_FMT_NV12);
            av_frame_unref(frame_);
            if (transferred) av_frame_unref(sw_frame_);
            continue;
        }

        // ========================
        // 提取 NV12 数据到连续内存
        // ========================
        auto nv12_data = extract_nv12(src_frame);

        // 构建 DecodedFrame
        DecodedFrame decoded;
        decoded.width = src_frame->width;
        decoded.height = src_frame->height;
        decoded.nv12_data = std::move(nv12_data);

        // PTS 和时间戳
        int64_t pts = frame_->pts;
        if (pts == AV_NOPTS_VALUE) {
            pts = frame_->best_effort_timestamp;
        }
        decoded.pts = pts;

        // 转换 PTS 到毫秒时间戳
        if (pts != AV_NOPTS_VALUE && video_stream_idx_ >= 0) {
            AVRational tb = fmt_ctx_->streams[video_stream_idx_]->time_base;
            decoded.timestamp_ms = av_rescale_q(pts, tb, {1, 1000});
        } else {
            // 使用系统时钟作为后备
            decoded.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // 清理 AVFrame
        av_frame_unref(frame_);
        if (transferred) av_frame_unref(sw_frame_);

        return decoded;
    }
}

bool HwDecoder::skip_frame() {
    if (!is_open_) {
        return false;
    }

    while (true) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            return false;
        }

        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN)) {
                LOG_WARN("Error sending packet to decoder, skipping");
            }
            continue;
        }

        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret < 0) {
            return false;
        }

        // 成功解码，直接丢弃帧数据（不做 HW transfer 和 NV12 extract）
        av_frame_unref(frame_);
        return true;
    }
}

std::shared_ptr<std::vector<uint8_t>> HwDecoder::extract_nv12(AVFrame* frame) {
    int w = frame->width;
    int h = frame->height;
    int y_size = w * h;
    int uv_size = w * (h / 2);

    auto buffer = std::make_shared<std::vector<uint8_t>>(y_size + uv_size);

    // 拷贝 Y 平面 (处理 stride/linesize)
    if (frame->linesize[0] == w) {
        // 无 padding, 直接整块拷贝
        std::memcpy(buffer->data(), frame->data[0], y_size);
    } else {
        // 有 padding, 逐行拷贝
        for (int row = 0; row < h; row++) {
            std::memcpy(buffer->data() + row * w,
                        frame->data[0] + row * frame->linesize[0],
                        w);
        }
    }

    // 拷贝 UV 平面 (NV12: UV interleaved, 高度为 h/2)
    if (frame->linesize[1] == w) {
        std::memcpy(buffer->data() + y_size, frame->data[1], uv_size);
    } else {
        for (int row = 0; row < h / 2; row++) {
            std::memcpy(buffer->data() + y_size + row * w,
                        frame->data[1] + row * frame->linesize[1],
                        w);
        }
    }

    return buffer;
}

void HwDecoder::close() {
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (sw_frame_) {
        av_frame_free(&sw_frame_);
        sw_frame_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    video_stream_idx_ = -1;
    width_ = 0;
    height_ = 0;
    fps_ = 0.0;
    codec_name_.clear();
    is_open_ = false;
    is_hw_decoder_ = false;
}

} // namespace infer_server

#endif // HAS_FFMPEG
