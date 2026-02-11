#pragma once

/**
 * @file rga_processor.h
 * @brief RGA (Raster Graphic Acceleration) 硬件图像处理
 *
 * 使用 Rockchip RGA 硬件加速进行:
 * - NV12 → RGB 色彩空间转换 + 缩放
 * - NV12 → NV12 缩放
 *
 * 所有操作使用虚拟地址模式 (wrapbuffer_virtualaddr)。
 * 输入输出均为 CPU 可访问的内存。
 */

#ifdef HAS_RGA

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>

namespace infer_server {

class RgaProcessor {
public:
    RgaProcessor() = default;
    ~RgaProcessor() = default;

    /// NV12 → RGB (resize + 色彩转换)
    /// @param nv12_data  NV12 数据 (Y + UV 连续, 大小 = src_w * src_h * 3/2)
    /// @param src_w      源宽度
    /// @param src_h      源高度
    /// @param dst_w      目标宽度
    /// @param dst_h      目标高度
    /// @return RGB 数据 (大小 = dst_w * dst_h * 3), 失败返回 nullptr
    static std::shared_ptr<std::vector<uint8_t>> nv12_to_rgb_resize(
        const uint8_t* nv12_data, int src_w, int src_h,
        int dst_w, int dst_h);

    /// NV12 → NV12 (仅缩放)
    /// @param nv12_data  NV12 数据 (Y + UV 连续)
    /// @param src_w      源宽度
    /// @param src_h      源高度
    /// @param dst_w      目标宽度
    /// @param dst_h      目标高度
    /// @return 缩放后的 NV12 数据 (大小 = dst_w * dst_h * 3/2), 失败返回 nullptr
    static std::shared_ptr<std::vector<uint8_t>> nv12_resize(
        const uint8_t* nv12_data, int src_w, int src_h,
        int dst_w, int dst_h);

    /// 根据目标宽度计算保持宽高比的目标高度 (偶数对齐)
    /// @param src_w      源宽度
    /// @param src_h      源高度
    /// @param target_w   目标宽度
    /// @return 保持宽高比的目标高度 (偶数)
    static int calc_proportional_height(int src_w, int src_h, int target_w);
};

} // namespace infer_server

#endif // HAS_RGA
