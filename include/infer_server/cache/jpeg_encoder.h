#pragma once

/**
 * @file jpeg_encoder.h
 * @brief JPEG 编码工具 (基于 libjpeg-turbo TurboJPEG API)
 *
 * 将 RGB 数据编码为 JPEG 格式, 用于图片缓存。
 * TurboJPEG 在 ARM64 上使用 NEON 指令优化, 编码速度很快。
 */

#ifdef HAS_TURBOJPEG

#include <vector>
#include <cstdint>
#include <memory>

namespace infer_server {

class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();

    // 禁止拷贝 (TurboJPEG handle 不可共享)
    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    /// 将 RGB 数据编码为 JPEG
    /// @param rgb_data   RGB888 数据 (3 bytes per pixel, R-G-B 顺序)
    /// @param width      图片宽度
    /// @param height     图片高度
    /// @param quality    JPEG 质量 (1-100, 推荐 70-85)
    /// @return JPEG 数据, 编码失败返回空 vector
    std::vector<uint8_t> encode(
        const uint8_t* rgb_data, int width, int height, int quality = 75);

    /// 编码器是否可用
    bool is_valid() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;  // tjhandle (TurboJPEG compressor handle)
};

} // namespace infer_server

#endif // HAS_TURBOJPEG
