#pragma once

/**
 * @file inference_engine.h
 * @brief 推理引擎 - 统一管理模型、工作线程、任务队列和 ZMQ 发布
 *
 * InferenceEngine 是 Phase 3 的核心编排组件:
 * - 拥有 ModelManager (模型生命周期)
 * - 拥有 BoundedQueue<InferTask> (全局推理任务队列)
 * - 拥有 N 个 InferWorker (NPU 推理线程)
 * - 拥有 ZmqPublisher (结果发布)
 *
 * 对外暴露简单接口:
 * - load_models(): 预加载模型
 * - submit(): 提交推理任务
 * - shutdown(): 优雅关闭
 *
 * StreamManager (Phase 4) 会调用这些接口来提交任务。
 */

#ifdef HAS_RKNN

#include "infer_server/common/config.h"
#include "infer_server/common/types.h"
#include "infer_server/common/bounded_queue.h"
#include "infer_server/inference/model_manager.h"
#include "infer_server/inference/infer_worker.h"

#ifdef HAS_ZMQ
#include "infer_server/output/zmq_publisher.h"
#endif

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <atomic>

namespace infer_server {

class InferenceEngine {
public:
    /// 外部结果回调 (除了 ZMQ 发布外的额外回调)
    using ResultCallback = std::function<void(const FrameResult&)>;

    /**
     * @brief 构造推理引擎
     * @param config 服务器配置
     */
    explicit InferenceEngine(const ServerConfig& config);

    ~InferenceEngine();

    // 禁止拷贝
    InferenceEngine(const InferenceEngine&) = delete;
    InferenceEngine& operator=(const InferenceEngine&) = delete;

    /**
     * @brief 初始化引擎 (创建工作线程、初始化 ZMQ)
     * @return true 初始化成功
     */
    bool init();

    /**
     * @brief 预加载模型列表
     *
     * 通常在添加流时调用, 确保流引用的所有模型已加载。
     * 已加载的模型不会重复加载。
     *
     * @param models 模型配置列表
     * @return true 所有模型加载成功
     */
    bool load_models(const std::vector<ModelConfig>& models);

    /**
     * @brief 提交推理任务
     *
     * 将 InferTask 推入全局有界队列。
     * 如果队列已满, 最旧的任务会被丢弃。
     *
     * @param task 推理任务
     * @return true 提交成功 (false 仅在引擎未初始化或已停止时)
     */
    bool submit(InferTask task);

    /**
     * @brief 优雅关闭引擎
     *
     * 停止所有工作线程, 关闭 ZMQ, 卸载模型。
     */
    void shutdown();

    /**
     * @brief 设置额外的结果回调
     *
     * 除了 ZMQ 发布外, 还可以设置额外回调 (如统计、日志等)。
     */
    void set_result_callback(ResultCallback cb) { result_callback_ = std::move(cb); }

    // ---- 状态查询 ----

    /// 引擎是否已初始化
    bool is_initialized() const { return initialized_.load(); }

    /// 任务队列当前大小
    size_t queue_size() const { return task_queue_.size(); }

    /// 任务队列丢弃计数
    size_t queue_dropped() const { return task_queue_.dropped_count(); }

    /// 模型管理器 (只读访问)
    const ModelManager& model_manager() const { return model_mgr_; }

    /// 模型管理器 (读写访问, 供 StreamManager 使用)
    ModelManager& model_manager() { return model_mgr_; }

    /// 工作线程数量
    size_t worker_count() const { return workers_.size(); }

    /// 总处理任务计数
    uint64_t total_processed() const;

#ifdef HAS_ZMQ
    /// ZMQ 已发布消息计数
    uint64_t zmq_published_count() const { return zmq_pub_.published_count(); }
#endif

private:
    /// 结果完成回调 (被 InferWorker 调用)
    void on_result_complete(FrameResult result);

    ServerConfig config_;
    ModelManager model_mgr_;
    BoundedQueue<InferTask> task_queue_;
    std::vector<std::unique_ptr<InferWorker>> workers_;

#ifdef HAS_ZMQ
    ZmqPublisher zmq_pub_;
#endif

    ResultCallback result_callback_;
    std::atomic<bool> initialized_{false};
};

} // namespace infer_server

#endif // HAS_RKNN
