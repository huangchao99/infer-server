#ifdef HAS_TURBOJPEG

#include "infer_server/cache/jpeg_encoder.h"
#include "infer_server/common/logger.h"

#include <turbojpeg.h>

namespace infer_server {

JpegEncoder::JpegEncoder() {
    handle_ = tjInitCompress();
    if (!handle_) {
        LOG_ERROR("Failed to initialize TurboJPEG compressor");
    }
}

JpegEncoder::~JpegEncoder() {
    if (handle_) {
        tjDestroy(handle_);
        handle_ = nullptr;
    }
}

std::vector<uint8_t> JpegEncoder::encode(
    const uint8_t* rgb_data, int width, int height, int quality)
{
    if (!handle_) {
        LOG_ERROR("JpegEncoder: compressor not initialized");
        return {};
    }

    if (!rgb_data || width <= 0 || height <= 0) {
        LOG_ERROR("JpegEncoder: invalid input ({}x{})", width, height);
        return {};
    }

    quality = std::max(1, std::min(100, quality));

    unsigned char* jpeg_buf = nullptr;
    unsigned long jpeg_size = 0;

    int ret = tjCompress2(
        static_cast<tjhandle>(handle_),
        rgb_data,
        width,
        0,          // pitch (0 = width * pixel_size)
        height,
        TJPF_RGB,   // 输入像素格式: RGB
        &jpeg_buf,
        &jpeg_size,
        TJSAMP_420, // 色度子采样: 4:2:0 (较小文件)
        quality,
        TJFLAG_FASTDCT  // 快速 DCT (稍微牺牲质量换速度)
    );

    if (ret != 0) {
        LOG_ERROR("JpegEncoder: tjCompress2 failed: {}", tjGetErrorStr2(static_cast<tjhandle>(handle_)));
        if (jpeg_buf) tjFree(jpeg_buf);
        return {};
    }

    // 拷贝到 vector
    std::vector<uint8_t> result(jpeg_buf, jpeg_buf + jpeg_size);

    // 释放 TurboJPEG 分配的缓冲区
    tjFree(jpeg_buf);

    LOG_TRACE("JPEG encoded: {}x{} q={} -> {} bytes", width, height, quality, result.size());
    return result;
}

} // namespace infer_server

#endif // HAS_TURBOJPEG
