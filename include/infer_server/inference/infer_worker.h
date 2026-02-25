#pragma once

/**
 * @file infer_worker.h
 * @brief NPU 推理工作线程
 *
 * 每个 InferWorker 运行在独立线程中, 绑定一个 NPU 核心:
 * 1. 从全局 BoundedQueue<InferTask> 竞争消费任务
 * 2. 使用 ModelManager 惰性创建 rknn_context
 * 3. 执行推理: rknn_inputs_set -> rknn_run -> rknn_outputs_get
 * 4. 调用 PostProcessor 进行 YOLO 后处理
 * 5. 通过 FrameResultCollector 聚合多模型结果
 * 6. 当帧的所有模型完成时, 调用 on_complete 回调
 */

#ifdef HAS_RKNN

#include "infer_server/inference/model_manager.h"
#include "infer_server/inference/post_processor.h"
#include "infer_server/inference/frame_result_collector.h"
#include "infer_server/common/types.h"
#include "infer_server/common/bounded_queue.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>

namespace infer_server {

class InferWorker {
public:
    /// 完成回调类型: 当帧的所有模型推理完成时调用
    using OnCompleteCallback = std::function<void(FrameResult)>;

    /**
     * @brief 构造推理工作线程
     *
     * @param worker_id   工作线程 ID (0, 1, 2, ...)
     * @param core_mask   NPU 核心掩码 (NpuCoreMask::CORE_0 等)
     * @param model_mgr   模型管理器引用 (共享)
     * @param task_queue   全局推理任务队列引用 (共享)
     * @param on_complete  帧结果完成回调
     */
    InferWorker(int worker_id, int core_mask,
                ModelManager& model_mgr,
                BoundedQueue<InferTask>& task_queue,
                OnCompleteCallback on_complete);

    ~InferWorker();

    // 禁止拷贝和移动
    InferWorker(const InferWorker&) = delete;
    InferWorker& operator=(const InferWorker&) = delete;

    /// 启动工作线程
    void start();

    /// 停止工作线程 (等待当前任务完成)
    void stop();

    /// 是否正在运行
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    /// Worker ID
    int worker_id() const { return worker_id_; }

    /// 已处理的任务计数
    uint64_t processed_count() const { return processed_count_.load(std::memory_order_relaxed); }

    /// 预创建模型的 rknn_context (在流启动前调用，避免与 RGA 并发导致硬件冲突)
    bool pre_create_context(const std::string& model_path);

private:
    /// 主循环
    void run();

    /// 处理单个推理任务
    void process_task(InferTask& task);

    /// 获取或创建模型的 rknn_context (惰性创建)
    rknn_context get_or_create_context(const std::string& model_path);

    /// 释放所有持有的 context
    void release_all_contexts();

    /// 加载标签文件
    static std::vector<std::string> load_labels(const std::string& labels_file);

    int worker_id_;
    int core_mask_;
    ModelManager& model_mgr_;
    BoundedQueue<InferTask>& task_queue_;
    OnCompleteCallback on_complete_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> processed_count_{0};

    /// 每个模型路径对应一个 rknn_context (惰性创建)
    std::unordered_map<std::string, rknn_context> contexts_;
};

} // namespace infer_server

#endif // HAS_RKNN
