#ifdef HAS_RGA

#include "infer_server/processor/rga_processor.h"
#include "infer_server/common/logger.h"

// RGA headers
// 尝试使用 im2d API (现代接口), 回退到 rga.h
#if __has_include(<rga/im2d.hpp>)
    #include <rga/im2d.hpp>
    #define RGA_USE_IM2D_HPP 1
#elif __has_include(<im2d.hpp>)
    #include <im2d.hpp>
    #define RGA_USE_IM2D_HPP 1
#elif __has_include(<rga/im2d.h>)
    extern "C" {
    #include <rga/im2d.h>
    }
    #define RGA_USE_IM2D_C 1
#elif __has_include(<im2d.h>)
    extern "C" {
    #include <im2d.h>
    }
    #define RGA_USE_IM2D_C 1
#endif

#if __has_include(<rga/rga.h>)
    extern "C" {
    #include <rga/rga.h>
    }
#elif __has_include(<rga.h>)
    extern "C" {
    #include <rga.h>
    }
#endif

#include <cstring>

namespace infer_server {

std::shared_ptr<std::vector<uint8_t>> RgaProcessor::nv12_to_rgb_resize(
    const uint8_t* nv12_data, int src_w, int src_h,
    int dst_w, int dst_h)
{
    if (!nv12_data || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        LOG_ERROR("RGA: invalid parameters src={}x{} dst={}x{}", src_w, src_h, dst_w, dst_h);
        return nullptr;
    }

    // RGA 要求宽高为偶数
    dst_w = (dst_w + 1) & ~1;
    dst_h = (dst_h + 1) & ~1;

    // 分配输出缓冲区 (RGB888: 3 bytes per pixel)
    size_t dst_size = static_cast<size_t>(dst_w) * dst_h * 3;
    auto rgb_buf = std::make_shared<std::vector<uint8_t>>(dst_size);

#if defined(RGA_USE_IM2D_HPP) || defined(RGA_USE_IM2D_C)
    // ========================
    // 使用 im2d API
    // ========================
    // wrapbuffer_virtualaddr 参数说明：
    //   vir_addr: 虚拟地址
    //   width, height: 图像的逻辑宽高
    //   format: 像素格式
    //   wstride: 每行步长（像素），通常等于 width
    //   hstride: 图像的虚拟高度（像素），对于 NV12 就是 height
    
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(nv12_data), src_w, src_h,
        RK_FORMAT_YCbCr_420_SP, src_w, src_h);

    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(
        rgb_buf->data(), dst_w, dst_h,
        RK_FORMAT_RGB_888, dst_w, dst_h);

    // 执行缩放 + 色彩转换
    // imresize 可以同时处理格式转换和缩放
    IM_STATUS status = imresize(src_buf, dst_buf);
    if (status != IM_STATUS_SUCCESS) {
        LOG_ERROR("RGA imresize (NV12->RGB) failed: {} (status={})", 
                  imStrError(status), static_cast<int>(status));
        return nullptr;
    }

    LOG_TRACE("RGA NV12({}x{}) -> RGB({}x{}) success", src_w, src_h, dst_w, dst_h);
    return rgb_buf;

#else
    LOG_ERROR("RGA im2d API not available. Need im2d.h or im2d.hpp");
    return nullptr;
#endif
}

std::shared_ptr<std::vector<uint8_t>> RgaProcessor::nv12_resize(
    const uint8_t* nv12_data, int src_w, int src_h,
    int dst_w, int dst_h)
{
    if (!nv12_data || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        LOG_ERROR("RGA: invalid parameters src={}x{} dst={}x{}", src_w, src_h, dst_w, dst_h);
        return nullptr;
    }

    dst_w = (dst_w + 1) & ~1;
    dst_h = (dst_h + 1) & ~1;

    // NV12 输出: Y + UV = w * h * 3/2
    size_t dst_size = static_cast<size_t>(dst_w) * dst_h * 3 / 2;
    auto nv12_out = std::make_shared<std::vector<uint8_t>>(dst_size);

#if defined(RGA_USE_IM2D_HPP) || defined(RGA_USE_IM2D_C)
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(nv12_data), src_w, src_h,
        RK_FORMAT_YCbCr_420_SP, src_w, src_h);

    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(
        nv12_out->data(), dst_w, dst_h,
        RK_FORMAT_YCbCr_420_SP, dst_w, dst_h);

    IM_STATUS status = imresize(src_buf, dst_buf);
    if (status != IM_STATUS_SUCCESS) {
        LOG_ERROR("RGA imresize (NV12->NV12) failed: {} (status={})", 
                  imStrError(status), static_cast<int>(status));
        return nullptr;
    }

    LOG_TRACE("RGA NV12({}x{}) -> NV12({}x{}) success", src_w, src_h, dst_w, dst_h);
    return nv12_out;

#else
    LOG_ERROR("RGA im2d API not available");
    return nullptr;
#endif
}

int RgaProcessor::calc_proportional_height(int src_w, int src_h, int target_w) {
    if (src_w <= 0 || src_h <= 0 || target_w <= 0) return 0;
    int h = (target_w * src_h) / src_w;
    // 对齐到偶数 (NV12 要求)
    return (h + 1) & ~1;
}

} // namespace infer_server

#endif // HAS_RGA
