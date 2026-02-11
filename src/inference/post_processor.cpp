/**
 * @file post_processor.cpp
 * @brief YOLO 后处理实现
 *
 * 支持 YOLOv5 (anchor-based) 和 YOLOv8/v11 (anchor-free DFL)。
 * 纯 CPU 计算，不依赖任何硬件。
 */

#include "infer_server/inference/post_processor.h"
#include "infer_server/common/logger.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cassert>

namespace infer_server {

// ============================================================
// 工具函数
// ============================================================

float PostProcessor::sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

float PostProcessor::iou(const BBox& a, const BBox& b) {
    float inter_x1 = std::max(a.x1, b.x1);
    float inter_y1 = std::max(a.y1, b.y1);
    float inter_x2 = std::min(a.x2, b.x2);
    float inter_y2 = std::min(a.y2, b.y2);

    float inter_area = std::max(0.0f, inter_x2 - inter_x1) *
                       std::max(0.0f, inter_y2 - inter_y1);
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - inter_area;

    return (union_area > 0.0f) ? (inter_area / union_area) : 0.0f;
}

void PostProcessor::dequantize_int8(const int8_t* data, float* output,
                                     int size, int32_t zp, float scale) {
    for (int i = 0; i < size; i++) {
        output[i] = (static_cast<float>(data[i]) - static_cast<float>(zp)) * scale;
    }
}

float PostProcessor::dfl_decode(const float* data, int reg_max) {
    // DFL: softmax over reg_max values, then compute weighted sum
    // result = sum(i * softmax(data[i])) for i in [0, reg_max)
    float max_val = data[0];
    for (int i = 1; i < reg_max; i++) {
        max_val = std::max(max_val, data[i]);
    }

    float sum_exp = 0.0f;
    std::vector<float> exp_vals(reg_max);
    for (int i = 0; i < reg_max; i++) {
        exp_vals[i] = std::exp(data[i] - max_val);
        sum_exp += exp_vals[i];
    }

    float result = 0.0f;
    for (int i = 0; i < reg_max; i++) {
        result += static_cast<float>(i) * (exp_vals[i] / sum_exp);
    }
    return result;
}

void PostProcessor::scale_coords(std::vector<Detection>& dets,
                                  int model_w, int model_h,
                                  int orig_w, int orig_h) {
    // letterbox 缩放: 等比例缩放，短边填充
    float scale = std::min(static_cast<float>(model_w) / orig_w,
                           static_cast<float>(model_h) / orig_h);
    float pad_x = (model_w - orig_w * scale) / 2.0f;
    float pad_y = (model_h - orig_h * scale) / 2.0f;

    for (auto& det : dets) {
        // 去除 padding，反缩放
        det.bbox.x1 = (det.bbox.x1 - pad_x) / scale;
        det.bbox.y1 = (det.bbox.y1 - pad_y) / scale;
        det.bbox.x2 = (det.bbox.x2 - pad_x) / scale;
        det.bbox.y2 = (det.bbox.y2 - pad_y) / scale;

        // 裁剪到原始图像范围
        det.bbox.x1 = std::max(0.0f, std::min(det.bbox.x1, static_cast<float>(orig_w)));
        det.bbox.y1 = std::max(0.0f, std::min(det.bbox.y1, static_cast<float>(orig_h)));
        det.bbox.x2 = std::max(0.0f, std::min(det.bbox.x2, static_cast<float>(orig_w)));
        det.bbox.y2 = std::max(0.0f, std::min(det.bbox.y2, static_cast<float>(orig_h)));
    }
}

// ============================================================
// NMS
// ============================================================

void PostProcessor::nms(std::vector<Detection>& detections, float threshold) {
    if (detections.empty()) return;

    // 按置信度降序排序
    std::sort(detections.begin(), detections.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<bool> suppressed(detections.size(), false);
    std::vector<Detection> result;
    result.reserve(detections.size());

    for (size_t i = 0; i < detections.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(detections[i]);

        for (size_t j = i + 1; j < detections.size(); j++) {
            if (suppressed[j]) continue;
            // 仅对同类进行 NMS
            if (detections[i].class_id == detections[j].class_id) {
                if (iou(detections[i].bbox, detections[j].bbox) > threshold) {
                    suppressed[j] = true;
                }
            }
        }
    }

    detections = std::move(result);
}

// ============================================================
// YOLOv5 后处理 (anchor-based)
// ============================================================

std::vector<Detection> PostProcessor::yolov5(
    const std::vector<float*>& outputs,
    const std::vector<TensorAttr>& attrs,
    int model_w, int model_h,
    int orig_w, int orig_h,
    float conf_thresh, float nms_thresh,
    const std::vector<std::string>& labels)
{
    if (outputs.size() != 3 || attrs.size() != 3) {
        LOG_ERROR("YOLOv5 expects 3 output heads, got {}", outputs.size());
        return {};
    }

    std::vector<Detection> all_detections;

    for (int head = 0; head < 3; head++) {
        const float* data = outputs[head];
        const auto& attr = attrs[head];

        if (attr.dims.size() < 4) {
            LOG_ERROR("YOLOv5 head {} expects 4D tensor, got {}D", head, attr.dims.size());
            continue;
        }

        // 输出 shape: [batch, grid_h, grid_w, num_anchors * (5 + num_classes)]
        int grid_h = attr.dims[1];
        int grid_w = attr.dims[2];
        int channel = attr.dims[3];
        int num_classes = channel / YOLOV5_NUM_ANCHORS - 5;

        if (num_classes <= 0) {
            LOG_ERROR("YOLOv5 head {}: invalid channel count {}, cannot determine num_classes",
                      head, channel);
            continue;
        }

        int stride = STRIDES[head];
        int entry_size = 5 + num_classes;

        for (int y = 0; y < grid_h; y++) {
            for (int x = 0; x < grid_w; x++) {
                for (int a = 0; a < YOLOV5_NUM_ANCHORS; a++) {
                    int offset = (y * grid_w * YOLOV5_NUM_ANCHORS + x * YOLOV5_NUM_ANCHORS + a) * entry_size;
                    const float* entry = data + offset;

                    float obj_conf = sigmoid(entry[4]);
                    if (obj_conf < conf_thresh) continue;

                    // 找最大类别置信度
                    int best_class = 0;
                    float best_score = entry[5];
                    for (int c = 1; c < num_classes; c++) {
                        if (entry[5 + c] > best_score) {
                            best_score = entry[5 + c];
                            best_class = c;
                        }
                    }
                    best_score = sigmoid(best_score);
                    float final_conf = obj_conf * best_score;
                    if (final_conf < conf_thresh) continue;

                    // 解码 box (cx, cy, w, h) -> (x1, y1, x2, y2)
                    float cx = (sigmoid(entry[0]) * 2.0f - 0.5f + static_cast<float>(x)) * stride;
                    float cy = (sigmoid(entry[1]) * 2.0f - 0.5f + static_cast<float>(y)) * stride;
                    float bw = std::pow(sigmoid(entry[2]) * 2.0f, 2.0f) * YOLOV5_ANCHORS[head][a * 2];
                    float bh = std::pow(sigmoid(entry[3]) * 2.0f, 2.0f) * YOLOV5_ANCHORS[head][a * 2 + 1];

                    Detection det;
                    det.class_id = best_class;
                    det.confidence = final_conf;
                    det.bbox.x1 = cx - bw / 2.0f;
                    det.bbox.y1 = cy - bh / 2.0f;
                    det.bbox.x2 = cx + bw / 2.0f;
                    det.bbox.y2 = cy + bh / 2.0f;

                    if (!labels.empty() && best_class < static_cast<int>(labels.size())) {
                        det.class_name = labels[best_class];
                    }

                    all_detections.push_back(det);
                }
            }
        }
    }

    // NMS
    nms(all_detections, nms_thresh);

    // 坐标映射到原始图像
    scale_coords(all_detections, model_w, model_h, orig_w, orig_h);

    return all_detections;
}

// ============================================================
// YOLOv8 后处理 (anchor-free, DFL, 3 个输出头)
// ============================================================

std::vector<Detection> PostProcessor::yolov8(
    const std::vector<float*>& outputs,
    const std::vector<TensorAttr>& attrs,
    int model_w, int model_h,
    int orig_w, int orig_h,
    float conf_thresh, float nms_thresh,
    const std::vector<std::string>& labels)
{
    if (outputs.size() != 3 || attrs.size() != 3) {
        LOG_ERROR("YOLOv8 expects 3 output heads, got {}", outputs.size());
        return {};
    }

    // YOLOv8/v11 RKNN 输出格式:
    // 每个 head shape: [1, grid_h, grid_w, 64 + num_classes]
    // 前 64 个通道: DFL box regression (4 * reg_max, reg_max=16)
    // 后 num_classes 个通道: class scores (已经过 sigmoid 或 raw)
    static constexpr int REG_MAX = 16;
    static constexpr int BOX_CHANNELS = 4 * REG_MAX;  // 64

    std::vector<Detection> all_detections;

    for (int head = 0; head < 3; head++) {
        const float* data = outputs[head];
        const auto& attr = attrs[head];

        if (attr.dims.size() < 4) {
            LOG_ERROR("YOLOv8 head {} expects 4D tensor, got {}D", head, attr.dims.size());
            continue;
        }

        int grid_h = attr.dims[1];
        int grid_w = attr.dims[2];
        int channel = attr.dims[3];
        int num_classes = channel - BOX_CHANNELS;

        if (num_classes <= 0) {
            LOG_ERROR("YOLOv8 head {}: channel={}, expected > {}", head, channel, BOX_CHANNELS);
            continue;
        }

        int stride = STRIDES[head];

        for (int y = 0; y < grid_h; y++) {
            for (int x = 0; x < grid_w; x++) {
                int offset = (y * grid_w + x) * channel;
                const float* entry = data + offset;

                // 先检查类别最大置信度 (score 在 box 后面)
                const float* scores = entry + BOX_CHANNELS;
                int best_class = 0;
                float best_score = scores[0];
                for (int c = 1; c < num_classes; c++) {
                    if (scores[c] > best_score) {
                        best_score = scores[c];
                        best_class = c;
                    }
                }

                // YOLOv8/v11 的 score 需要 sigmoid
                best_score = sigmoid(best_score);
                if (best_score < conf_thresh) continue;

                // DFL 解码 box: 4 组 reg_max 值 -> (left, top, right, bottom) 距离
                float left   = dfl_decode(entry + 0 * REG_MAX, REG_MAX) * stride;
                float top    = dfl_decode(entry + 1 * REG_MAX, REG_MAX) * stride;
                float right  = dfl_decode(entry + 2 * REG_MAX, REG_MAX) * stride;
                float bottom = dfl_decode(entry + 3 * REG_MAX, REG_MAX) * stride;

                // grid 中心点
                float cx = (static_cast<float>(x) + 0.5f) * stride;
                float cy = (static_cast<float>(y) + 0.5f) * stride;

                Detection det;
                det.class_id = best_class;
                det.confidence = best_score;
                det.bbox.x1 = cx - left;
                det.bbox.y1 = cy - top;
                det.bbox.x2 = cx + right;
                det.bbox.y2 = cy + bottom;

                if (!labels.empty() && best_class < static_cast<int>(labels.size())) {
                    det.class_name = labels[best_class];
                }

                all_detections.push_back(det);
            }
        }
    }

    // NMS
    nms(all_detections, nms_thresh);

    // 坐标映射到原始图像
    scale_coords(all_detections, model_w, model_h, orig_w, orig_h);

    return all_detections;
}

// ============================================================
// YOLOv11 后处理 (anchor-free, 单输出头融合格式)
//
// 实际输出格式（经诊断验证）：
//   - bbox 分支：模型内部已完成 DFL解码 + anchor解码，直接输出绝对坐标 [x1,y1,x2,y2]
//   - cls  分支：输出 logits (raw scores)，需要经过 sigmoid 转为概率 [0,1]
//   - 内存布局：[N, A, C] = [1, 8400, 84]
//     每 84 个连续 float 是一个 anchor 的完整数据：
//     [x1, y1, x2, y2, cls0_logit, cls1_logit, ..., cls79_logit]
//   - 注意bbox格式为[cx, cy, w, h] 格式不是[x1, y1, x2, y2]
// ============================================================


std::vector<Detection> PostProcessor::yolov11(
    const std::vector<float*>& outputs,
    const std::vector<TensorAttr>& attrs,
    int model_w, int model_h,
    int orig_w,  int orig_h,
    float conf_thresh, float nms_thresh,
    const std::vector<std::string>& labels)
{
    // 1. 基础校验
    if (outputs.empty() || attrs.empty()) {
        LOG_ERROR("YOLOv11: No output data");
        return {};
    }

    const float* data = outputs[0];
    const auto&  attr = attrs[0];

    // 预期 dims: [1, 84, 8400]
    const int num_channels = attr.dims[1]; // 84
    const int num_anchors  = attr.dims[2]; // 8400
    const int num_classes  = num_channels - 4;

    std::vector<Detection> all_detections;
    all_detections.reserve(200); // 预估检测物数量

    // 2. 遍历所有 Anchor 点 (8400个)
    for (int i = 0; i < num_anchors; i++) {
        
        // ── A. 寻找最高置信度类别 ─────────────────────────────────────
        // 类别通道从 Index 4 开始到 83
        // 内存布局为 NCHW，所以访问方式是 data[(通道号) * num_anchors + anchor索引]
        int   best_class = -1;
        float best_score = -1.0f;

        for (int c = 0; c < num_classes; c++) {
            float score = data[(4 + c) * num_anchors + i];
            if (score > best_score) {
                best_score = score;
                best_class = c;
            }
        }

        // 阈值过滤 (根据实验，score 已经是概率值，无需再做 sigmoid)
        if (best_score < conf_thresh) continue;

        // ── B. 读取并转换 bbox 坐标 ──────────────────────────────────
        // 根据实验结果，Index 0-3 分别是 cx, cy, w, h
        float cx = data[0 * num_anchors + i];
        float cy = data[1 * num_anchors + i];
        float w  = data[2 * num_anchors + i];
        float h  = data[3 * num_anchors + i];

        // 转换 [cx, cy, w, h] -> [x1, y1, x2, y2]
        float x1 = cx - w * 0.5f;
        float y1 = cy - h * 0.5f;
        float x2 = cx + w * 0.5f;
        float y2 = cy + h * 0.5f;

        // ── C. 构造检测结果 ──────────────────────────────────────
        Detection det;
        det.class_id   = best_class;
        det.confidence = best_score;
        det.bbox.x1    = x1;
        det.bbox.y1    = y1;
        det.bbox.x2    = x2;
        det.bbox.y2    = y2;

        if (!labels.empty() && best_class < (int)labels.size()) {
            det.class_name = labels[best_class];
        }

        all_detections.push_back(det);
    }

    LOG_DEBUG("YOLOv11: {} candidates before NMS", all_detections.size());

    // 3. 非极大值抑制 (NMS)
    // 这一步非常重要，会把你实验中看到的“重叠框”滤掉
    nms(all_detections, nms_thresh);

    LOG_DEBUG("YOLOv11: {} detections after NMS", all_detections.size());

    // 4. 坐标映射
    // 将 640x640 尺度下的坐标缩放到原始图像尺寸 (orig_w, orig_h)
    scale_coords(all_detections, model_w, model_h, orig_w, orig_h);

    return all_detections;
}

// ============================================================
// 统一分发
// ============================================================

std::vector<Detection> PostProcessor::process(
    const std::string& model_type,
    const std::vector<float*>& outputs,
    const std::vector<TensorAttr>& attrs,
    int model_w, int model_h,
    int orig_w, int orig_h,
    float conf_thresh, float nms_thresh,
    const std::vector<std::string>& labels)
{
    if (model_type == "yolov5") {
        return yolov5(outputs, attrs, model_w, model_h, orig_w, orig_h,
                      conf_thresh, nms_thresh, labels);
    } else if (model_type == "yolov8") {
        return yolov8(outputs, attrs, model_w, model_h, orig_w, orig_h,
                      conf_thresh, nms_thresh, labels);
    } else if (model_type == "yolov11") {
        // YOLOv11 使用单输出头融合格式
        return yolov11(outputs, attrs, model_w, model_h, orig_w, orig_h,
                       conf_thresh, nms_thresh, labels);
    } else {
        LOG_ERROR("Unknown model type: '{}', supported: yolov5, yolov8, yolov11", model_type);
        return {};
    }
}

} // namespace infer_server
