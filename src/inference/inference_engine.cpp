/**
 * @file inference_engine.cpp
 * @brief 推理引擎实现
 */

#ifdef HAS_RKNN

#include "infer_server/inference/inference_engine.h"
#include "infer_server/common/logger.h"

namespace infer_server {

InferenceEngine::InferenceEngine(const ServerConfig& config)
    : config_(config)
    , task_queue_(static_cast<size_t>(config.infer_queue_size))
#ifdef HAS_ZMQ
    , zmq_pub_(config.zmq_endpoint)
#endif
{
}

InferenceEngine::~InferenceEngine() {
    shutdown();
}

bool InferenceEngine::init() {
    if (initialized_.load()) {
        LOG_WARN("InferenceEngine already initialized");
        return true;
    }

    LOG_INFO("Initializing InferenceEngine...");
    LOG_INFO("  Workers:    {}", config_.num_infer_workers);
    LOG_INFO("  Queue size: {}", config_.infer_queue_size);

#ifdef HAS_ZMQ
    // 初始化 ZMQ
    LOG_INFO("  ZMQ endpoint: {}", config_.zmq_endpoint);
    if (!zmq_pub_.init()) {
        LOG_ERROR("Failed to initialize ZMQ publisher");
        return false;
    }
#else
    LOG_WARN("ZMQ not available, results will only be passed via callback");
#endif

    // 创建工作线程
    int num_workers = config_.num_infer_workers;
    int num_npu_cores = config_.num_npu_cores;
    LOG_INFO("  NPU cores: {}", num_npu_cores);
    workers_.reserve(num_workers);

    for (int i = 0; i < num_workers; i++) {
        int core_mask = NpuCoreMask::from_worker_id(i, num_npu_cores);
        auto worker = std::make_unique<InferWorker>(
            i, core_mask, model_mgr_, task_queue_,
            [this](FrameResult result) {
                on_result_complete(std::move(result));
            }
        );
        workers_.push_back(std::move(worker));
    }

    // 启动所有工作线程
    for (auto& worker : workers_) {
        worker->start();
    }

    initialized_ = true;
    LOG_INFO("InferenceEngine initialized with {} workers", num_workers);
    return true;
}

bool InferenceEngine::load_models(const std::vector<ModelConfig>& models) {
    bool all_ok = true;
    for (const auto& mc : models) {
        if (!model_mgr_.is_loaded(mc.model_path)) {
            LOG_INFO("Pre-loading model: {} (task={})", mc.model_path, mc.task_name);
            if (!model_mgr_.load_model(mc.model_path)) {
                LOG_ERROR("Failed to load model: {}", mc.model_path);
                all_ok = false;
                continue;
            }
            // 预创建所有 worker context，避免惰性创建时与 RGA 硬件并发冲突
            for (auto& worker : workers_) {
                if (!worker->pre_create_context(mc.model_path)) {
                    LOG_ERROR("Failed to pre-create context for worker {}", worker->worker_id());
                    all_ok = false;
                }
            }
        }
    }
    return all_ok;
}

bool InferenceEngine::submit(InferTask task) {
    if (!initialized_.load()) {
        LOG_WARN("InferenceEngine not initialized, dropping task");
        return false;
    }
    return task_queue_.push(std::move(task));
}

void InferenceEngine::shutdown() {
    if (!initialized_.load()) return;

    LOG_INFO("InferenceEngine shutting down...");

    // 停止任务队列 (唤醒等待中的 worker)
    task_queue_.stop();

    // 停止所有工作线程
    for (auto& worker : workers_) {
        worker->stop();
    }
    workers_.clear();

#ifdef HAS_ZMQ
    zmq_pub_.shutdown();
#endif

    // 卸载所有模型
    model_mgr_.unload_all();

    initialized_ = false;
    LOG_INFO("InferenceEngine shutdown complete");
}

uint64_t InferenceEngine::total_processed() const {
    uint64_t total = 0;
    for (const auto& w : workers_) {
        total += w->processed_count();
    }
    return total;
}

void InferenceEngine::on_result_complete(FrameResult result) {
    // 1. ZMQ 发布
#ifdef HAS_ZMQ
    zmq_pub_.publish(result);
#endif

    // 2. 额外回调
    if (result_callback_) {
        result_callback_(result);
    }
}

} // namespace infer_server

#endif // HAS_RKNN
