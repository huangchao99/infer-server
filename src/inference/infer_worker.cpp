/**
 * @file infer_worker.cpp
 * @brief NPU 推理工作线程实现
 */

#ifdef HAS_RKNN

#include "infer_server/inference/infer_worker.h"
#include "infer_server/common/logger.h"
#include <fstream>
#include <cstring>
#include <chrono>

namespace infer_server {

// ============================================================
// 构造/析构
// ============================================================

InferWorker::InferWorker(int worker_id, int core_mask,
                         ModelManager& model_mgr,
                         BoundedQueue<InferTask>& task_queue,
                         OnCompleteCallback on_complete)
    : worker_id_(worker_id)
    , core_mask_(core_mask)
    , model_mgr_(model_mgr)
    , task_queue_(task_queue)
    , on_complete_(std::move(on_complete))
{
}

InferWorker::~InferWorker() {
    stop();
}

// ============================================================
// 启动/停止
// ============================================================

void InferWorker::start() {
    if (running_.load()) return;

    stop_requested_ = false;
    running_ = true;
    thread_ = std::thread(&InferWorker::run, this);

    LOG_INFO("InferWorker[{}] started (core_mask={})", worker_id_, core_mask_);
}

void InferWorker::stop() {
    if (!running_.load()) return;

    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;

    release_all_contexts();
    LOG_INFO("InferWorker[{}] stopped (processed {} tasks)", worker_id_, processed_count_.load());
}

// ============================================================
// 主循环
// ============================================================

void InferWorker::run() {
    LOG_DEBUG("InferWorker[{}] thread started", worker_id_);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // 阻塞等待任务, 500ms 超时后检查 stop 信号
        auto task_opt = task_queue_.pop(std::chrono::milliseconds(500));
        if (!task_opt) continue;

        process_task(*task_opt);
        processed_count_.fetch_add(1, std::memory_order_relaxed);
    }

    LOG_DEBUG("InferWorker[{}] thread exiting", worker_id_);
}

// ============================================================
// 处理单个任务
// ============================================================

void InferWorker::process_task(InferTask& task) {
    auto t_start = std::chrono::steady_clock::now();

    // 1. 获取 rknn_context
    rknn_context ctx = get_or_create_context(task.model_path);
    if (ctx == 0) {
        LOG_ERROR("InferWorker[{}]: cannot get context for model: {}",
                  worker_id_, task.model_path);
        return;
    }

    // 2. 获取模型信息
    const ModelInfo* model_info = model_mgr_.get_model_info(task.model_path);
    if (!model_info) {
        LOG_ERROR("InferWorker[{}]: model info not found: {}", worker_id_, task.model_path);
        return;
    }

    // 3. 设置输入
    rknn_input inputs[1];
    std::memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = task.input_data ? task.input_data->size() : 0;
    inputs[0].buf = task.input_data ? task.input_data->data() : nullptr;
    inputs[0].pass_through = 0;

    if (inputs[0].size == 0 || inputs[0].buf == nullptr) {
        LOG_ERROR("InferWorker[{}]: empty input data for task [{}] frame {}",
                  worker_id_, task.cam_id, task.frame_id);
        return;
    }

    int ret = rknn_inputs_set(ctx, 1, inputs);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("InferWorker[{}]: rknn_inputs_set failed: ret={}", worker_id_, ret);
        return;
    }

    // 4. 推理
    ret = rknn_run(ctx, nullptr);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("InferWorker[{}]: rknn_run failed: ret={}", worker_id_, ret);
        return;
    }

    // 5. 获取输出 (float 格式)
    uint32_t n_output = model_info->io_num.n_output;
    std::vector<rknn_output> rknn_outputs(n_output);
    for (uint32_t i = 0; i < n_output; i++) {
        std::memset(&rknn_outputs[i], 0, sizeof(rknn_output));
        rknn_outputs[i].want_float = 1;
    }

    ret = rknn_outputs_get(ctx, n_output, rknn_outputs.data(), nullptr);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("InferWorker[{}]: rknn_outputs_get failed: ret={}", worker_id_, ret);
        return;
    }

    auto t_infer_done = std::chrono::steady_clock::now();

    // 6. 后处理 (纯 CPU，不需要 RKNN 锁)
    std::vector<float*> output_ptrs(n_output);
    for (uint32_t i = 0; i < n_output; i++) {
        output_ptrs[i] = static_cast<float*>(rknn_outputs[i].buf);
    }

    auto tensor_attrs = model_info->get_output_tensor_attrs();

    auto detections = PostProcessor::process(
        task.model_type,
        output_ptrs,
        tensor_attrs,
        task.input_width, task.input_height,
        task.original_width, task.original_height,
        task.conf_threshold, task.nms_threshold,
        task.labels
    );

    rknn_outputs_release(ctx, n_output, rknn_outputs.data());

    auto t_post_done = std::chrono::steady_clock::now();

    double infer_ms = std::chrono::duration<double, std::milli>(t_infer_done - t_start).count();
    double post_ms = std::chrono::duration<double, std::milli>(t_post_done - t_infer_done).count();
    double total_ms = std::chrono::duration<double, std::milli>(t_post_done - t_start).count();

    LOG_DEBUG("InferWorker[{}]: [{}] frame {} model={} -> {} dets "
              "(infer={:.1f}ms post={:.1f}ms total={:.1f}ms)",
              worker_id_, task.cam_id, task.frame_id,
              task.task_name, detections.size(),
              infer_ms, post_ms, total_ms);

    // 7. 构造 ModelResult
    ModelResult model_result;
    model_result.task_name = task.task_name;
    model_result.model_path = task.model_path;
    model_result.inference_time_ms = total_ms;
    model_result.detections = std::move(detections);

    // 8. 聚合结果
    if (task.aggregator) {
        auto* collector = static_cast<FrameResultCollector*>(task.aggregator.get());
        auto complete_result = collector->add_result(std::move(model_result));
        if (complete_result && on_complete_) {
            on_complete_(std::move(*complete_result));
        }
    } else {
        // 单模型场景, 无 aggregator, 直接组装 FrameResult
        FrameResult result;
        result.cam_id = task.cam_id;
        result.rtsp_url = task.rtsp_url;
        result.frame_id = task.frame_id;
        result.timestamp_ms = task.timestamp_ms;
        result.pts = task.pts;
        result.original_width = task.original_width;
        result.original_height = task.original_height;
        result.results.push_back(std::move(model_result));

        if (on_complete_) {
            on_complete_(std::move(result));
        }
    }
}

// ============================================================
// Context 管理
// ============================================================

bool InferWorker::pre_create_context(const std::string& model_path) {
    if (contexts_.count(model_path)) return true;

    LOG_INFO("InferWorker[{}]: pre-creating context for model: {}", worker_id_, model_path);
    rknn_context ctx = model_mgr_.create_worker_context(model_path, core_mask_);
    if (ctx == 0) {
        LOG_ERROR("InferWorker[{}]: failed to pre-create context for: {}", worker_id_, model_path);
        return false;
    }
    contexts_[model_path] = ctx;
    return true;
}

rknn_context InferWorker::get_or_create_context(const std::string& model_path) {
    auto it = contexts_.find(model_path);
    if (it != contexts_.end()) {
        return it->second;
    }

    // 惰性创建
    LOG_INFO("InferWorker[{}]: creating context for model: {}", worker_id_, model_path);
    rknn_context ctx = model_mgr_.create_worker_context(model_path, core_mask_);
    if (ctx == 0) {
        return 0;
    }

    contexts_[model_path] = ctx;
    return ctx;
}

void InferWorker::release_all_contexts() {
    for (auto& [path, ctx] : contexts_) {
        LOG_DEBUG("InferWorker[{}]: releasing context for model: {}", worker_id_, path);
        model_mgr_.release_worker_context(ctx);
    }
    contexts_.clear();
}

// ============================================================
// 标签文件加载
// ============================================================

std::vector<std::string> InferWorker::load_labels(const std::string& labels_file) {
    std::vector<std::string> labels;
    if (labels_file.empty()) return labels;

    std::ifstream file(labels_file);
    if (!file.is_open()) {
        LOG_WARN("Cannot open labels file: {}", labels_file);
        return labels;
    }

    std::string line;
    while (std::getline(file, line)) {
        // 去除尾部空白
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            labels.push_back(line);
        }
    }

    LOG_DEBUG("Loaded {} labels from {}", labels.size(), labels_file);
    return labels;
}

} // namespace infer_server

#endif // HAS_RKNN
