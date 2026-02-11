#pragma once

/**
 * @file frame_result_collector.h
 * @brief 多模型推理结果聚合器 (header-only)
 *
 * 当一帧需要多个模型推理时 (如同时检测手机和吸烟),
 * 解码线程为该帧创建一个 FrameResultCollector,
 * 所有相关的 InferTask 共享同一个 Collector 指针。
 *
 * 多个 InferWorker 线程可并发调用 add_result(),
 * 当最后一个模型完成时, add_result() 返回完整的 FrameResult。
 *
 * 用法:
 *   // 解码线程
 *   auto collector = std::make_shared<FrameResultCollector>(num_models, base_result);
 *   for (auto& model : models) {
 *       InferTask task;
 *       task.aggregator = collector;
 *       queue.push(std::move(task));
 *   }
 *
 *   // InferWorker 线程
 *   auto* collector = static_cast<FrameResultCollector*>(task.aggregator.get());
 *   auto complete = collector->add_result(model_result);
 *   if (complete) {
 *       zmq_publisher.publish(*complete);
 *   }
 */

#include "infer_server/common/types.h"
#include <mutex>
#include <atomic>
#include <optional>
#include <vector>

namespace infer_server {

class FrameResultCollector {
public:
    /**
     * @brief 构造聚合器
     * @param total_models 需要等待的模型总数
     * @param base_result  基础帧信息 (cam_id, frame_id, timestamp 等)
     */
    FrameResultCollector(int total_models, FrameResult base_result)
        : total_models_(total_models)
        , result_(std::move(base_result))
    {
        result_.results.reserve(total_models);
    }

    // 禁止拷贝
    FrameResultCollector(const FrameResultCollector&) = delete;
    FrameResultCollector& operator=(const FrameResultCollector&) = delete;

    /**
     * @brief 添加一个模型的推理结果 (线程安全)
     *
     * @param model_result 单个模型的推理结果
     * @return 当所有模型都完成时，返回完整的 FrameResult；否则返回 nullopt
     */
    std::optional<FrameResult> add_result(ModelResult model_result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_.results.push_back(std::move(model_result));
        int completed = completed_.fetch_add(1, std::memory_order_relaxed) + 1;

        if (completed == total_models_) {
            return result_;
        }
        return std::nullopt;
    }

    /// 模型总数
    int total_models() const { return total_models_; }

    /// 已完成的模型数
    int completed_count() const {
        return completed_.load(std::memory_order_relaxed);
    }

    /// 是否已全部完成
    bool is_complete() const {
        return completed_.load(std::memory_order_relaxed) >= total_models_;
    }

private:
    int total_models_;
    FrameResult result_;
    std::atomic<int> completed_{0};
    std::mutex mutex_;
};

} // namespace infer_server
