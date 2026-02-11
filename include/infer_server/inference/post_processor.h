#pragma once

/**
 * @file post_processor.h
 * @brief YOLO 模型后处理器
 *
 * 支持 YOLOv5 (anchor-based) 和 YOLOv8/v11 (anchor-free) 两种格式。
 * 纯 CPU 计算，不依赖任何硬件库，可在任何平台上测试。
 *
 * RKNN 模型输出格式说明:
 *   YOLOv5: 3 个输出头 (stride 8/16/32)
 *     - 每个输出 shape: [1, grid_h, grid_w, num_anchors * (5 + num_classes)]
 *     - 5 = (cx, cy, w, h, obj_conf)
 *   YOLOv8/v11: 3 个输出头 (stride 8/16/32), anchor-free
 *     - box output:  [1, grid_h, grid_w, 64]  (DFL box regression, 4 * reg_max=16)
 *     - score output: [1, grid_h, grid_w, num_classes]
 *     - 或合并格式: [1, grid_h, grid_w, 64 + num_classes]
 */

#include "infer_server/common/types.h"
#include <vector>
#include <string>
#include <cstdint>

namespace infer_server {

/**
 * @brief YOLO 后处理输出的 tensor 信息
 *
 * 用于将 RKNN 输出 tensor 描述传递给后处理函数，
 * 不直接依赖 rknn_api.h，方便独立测试。
 */
struct TensorAttr {
    int n_elems = 0;          ///< 元素总数
    std::vector<int> dims;    ///< 维度 (如 [1, 80, 80, 255])

    // 量化参数 (INT8 反量化用)
    int32_t zp = 0;           ///< zero point
    float scale = 1.0f;       ///< scale factor
    bool is_int8 = false;     ///< 是否为 INT8 量化数据
};

/**
 * @brief YOLO 后处理器
 */
class PostProcessor {
public:
    /**
     * @brief YOLOv5 后处理 (anchor-based)
     *
     * @param outputs     3 个输出头的 float 数据指针
     * @param attrs       3 个输出头的 tensor 属性
     * @param model_w     模型输入宽度
     * @param model_h     模型输入高度
     * @param orig_w      原始帧宽度
     * @param orig_h      原始帧高度
     * @param conf_thresh 置信度阈值
     * @param nms_thresh  NMS 阈值
     * @param labels      类别标签列表
     * @return 检测结果列表
     */
    static std::vector<Detection> yolov5(
        const std::vector<float*>& outputs,
        const std::vector<TensorAttr>& attrs,
        int model_w, int model_h,
        int orig_w, int orig_h,
        float conf_thresh, float nms_thresh,
        const std::vector<std::string>& labels);

    /**
     * @brief YOLOv8 后处理 (anchor-free, DFL, 3 个输出头)
     *
     * @param outputs     3 个输出头的 float 数据指针 (每头: box+score 合并)
     * @param attrs       3 个输出头的 tensor 属性
     * @param model_w     模型输入宽度
     * @param model_h     模型输入高度
     * @param orig_w      原始帧宽度
     * @param orig_h      原始帧高度
     * @param conf_thresh 置信度阈值
     * @param nms_thresh  NMS 阈值
     * @param labels      类别标签列表
     * @return 检测结果列表
     */
    static std::vector<Detection> yolov8(
        const std::vector<float*>& outputs,
        const std::vector<TensorAttr>& attrs,
        int model_w, int model_h,
        int orig_w, int orig_h,
        float conf_thresh, float nms_thresh,
        const std::vector<std::string>& labels);

    /**
     * @brief YOLOv11 后处理 (anchor-free, 单输出头融合格式)
     *
     * @param outputs     1 个输出头的 float 数据指针, shape: [1, num_classes+4, num_anchors]
     * @param attrs       1 个输出头的 tensor 属性
     * @param model_w     模型输入宽度
     * @param model_h     模型输入高度
     * @param orig_w      原始帧宽度
     * @param orig_h      原始帧高度
     * @param conf_thresh 置信度阈值
     * @param nms_thresh  NMS 阈值
     * @param labels      类别标签列表
     * @return 检测结果列表
     */
    static std::vector<Detection> yolov11(
        const std::vector<float*>& outputs,
        const std::vector<TensorAttr>& attrs,
        int model_w, int model_h,
        int orig_w, int orig_h,
        float conf_thresh, float nms_thresh,
        const std::vector<std::string>& labels);

    /**
     * @brief 通用 NMS (Non-Maximum Suppression)
     * @param detections 检测结果 (会被修改, 保留 NMS 后的结果)
     * @param threshold  IoU 阈值
     */
    static void nms(std::vector<Detection>& detections, float threshold);

    /**
     * @brief INT8 反量化到 float
     * @param data   INT8 数据
     * @param output float 输出
     * @param size   元素数量
     * @param zp     zero point
     * @param scale  scale factor
     */
    static void dequantize_int8(const int8_t* data, float* output,
                                int size, int32_t zp, float scale);

    /**
     * @brief 根据模型类型字符串分发后处理
     * @param model_type "yolov5", "yolov8", "yolov11"
     */
    static std::vector<Detection> process(
        const std::string& model_type,
        const std::vector<float*>& outputs,
        const std::vector<TensorAttr>& attrs,
        int model_w, int model_h,
        int orig_w, int orig_h,
        float conf_thresh, float nms_thresh,
        const std::vector<std::string>& labels);

private:
    /// YOLOv5 默认 anchor 定义 (COCO)
    static constexpr float YOLOV5_ANCHORS[3][6] = {
        {10.0f, 13.0f, 16.0f,  30.0f,  33.0f,  23.0f},   // stride 8
        {30.0f, 61.0f, 62.0f,  45.0f,  59.0f, 119.0f},   // stride 16
        {116.0f, 90.0f, 156.0f, 198.0f, 373.0f, 326.0f}  // stride 32
    };

    static constexpr int YOLOV5_NUM_ANCHORS = 3;
    static constexpr int STRIDES[3] = {8, 16, 32};

    /// DFL (Distribution Focal Loss) softmax 解码
    static float dfl_decode(const float* data, int reg_max);

    /// Sigmoid 函数
    static float sigmoid(float x);

    /// 计算两个 BBox 的 IoU
    static float iou(const BBox& a, const BBox& b);

    /// 将检测框从模型坐标映射到原始图像坐标
    /// 假设使用 letterbox (等比例缩放+填充) 预处理
    static void scale_coords(std::vector<Detection>& dets,
                             int model_w, int model_h,
                             int orig_w, int orig_h);
};

} // namespace infer_server
