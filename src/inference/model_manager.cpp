/**
 * @file model_manager.cpp
 * @brief RKNN 模型管理器实现
 */

#ifdef HAS_RKNN

#include "infer_server/inference/model_manager.h"
#include "infer_server/common/logger.h"
#include <fstream>
#include <cstring>

namespace infer_server {

// ============================================================
// ModelInfo
// ============================================================

std::vector<TensorAttr> ModelInfo::get_output_tensor_attrs() const {
    std::vector<TensorAttr> attrs;
    attrs.reserve(output_attrs.size());

    for (const auto& rknn_attr : output_attrs) {
        TensorAttr attr;
        attr.n_elems = rknn_attr.n_elems;

        // 提取维度信息 (NHWC 格式)
        for (uint32_t i = 0; i < rknn_attr.n_dims; i++) {
            attr.dims.push_back(static_cast<int>(rknn_attr.dims[i]));
        }

        // 量化参数
        if (rknn_attr.type == RKNN_TENSOR_INT8) {
            attr.is_int8 = true;
            attr.zp = rknn_attr.zp;
            attr.scale = rknn_attr.scale;
        }

        attrs.push_back(attr);
    }

    return attrs;
}

// ============================================================
// ModelManager
// ============================================================

ModelManager::~ModelManager() {
    unload_all();
}

bool ModelManager::load_model(const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 已加载
    if (models_.count(model_path)) {
        LOG_DEBUG("Model already loaded: {}", model_path);
        return true;
    }

    // 读取模型文件
    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open model file: {}", model_path);
        return false;
    }

    auto file_size = file.tellg();
    if (file_size <= 0) {
        LOG_ERROR("Model file is empty: {}", model_path);
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> model_data(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(model_data.data()), file_size)) {
        LOG_ERROR("Failed to read model file: {}", model_path);
        return false;
    }
    file.close();

    LOG_INFO("Loading RKNN model: {} ({:.2f} MB)", model_path,
             static_cast<double>(file_size) / (1024.0 * 1024.0));

    // rknn_init
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model_data.data(), static_cast<uint32_t>(model_data.size()),
                        0, nullptr);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("rknn_init failed for {}: ret={}", model_path, ret);
        return false;
    }

    // 查询 IO 数量
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        LOG_ERROR("rknn_query IN_OUT_NUM failed: ret={}", ret);
        rknn_destroy(ctx);
        return false;
    }

    LOG_INFO("  Inputs: {}, Outputs: {}", io_num.n_input, io_num.n_output);

    // 查询输入属性
    std::vector<rknn_tensor_attr> input_attrs(io_num.n_input);
    for (uint32_t i = 0; i < io_num.n_input; i++) {
        std::memset(&input_attrs[i], 0, sizeof(rknn_tensor_attr));
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            LOG_ERROR("rknn_query INPUT_ATTR[{}] failed: ret={}", i, ret);
            rknn_destroy(ctx);
            return false;
        }
        LOG_INFO("  Input[{}]: fmt={} type={} dims=[{},{},{},{}] n_elems={}",
                 i, input_attrs[i].fmt, input_attrs[i].type,
                 input_attrs[i].dims[0], input_attrs[i].dims[1],
                 input_attrs[i].dims[2], input_attrs[i].dims[3],
                 input_attrs[i].n_elems);
    }

    // 查询输出属性
    std::vector<rknn_tensor_attr> output_attrs(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        std::memset(&output_attrs[i], 0, sizeof(rknn_tensor_attr));
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            LOG_ERROR("rknn_query OUTPUT_ATTR[{}] failed: ret={}", i, ret);
            rknn_destroy(ctx);
            return false;
        }
        LOG_INFO("  Output[{}]: fmt={} type={} dims=[{},{},{},{}] n_elems={} zp={} scale={:.6f}",
                 i, output_attrs[i].fmt, output_attrs[i].type,
                 output_attrs[i].dims[0], output_attrs[i].dims[1],
                 output_attrs[i].dims[2], output_attrs[i].dims[3],
                 output_attrs[i].n_elems,
                 output_attrs[i].zp, output_attrs[i].scale);
    }

    // 存储
    LoadedModel loaded;
    loaded.master_ctx = ctx;
    loaded.model_data = std::move(model_data);
    loaded.info.model_path = model_path;
    loaded.info.io_num = io_num;
    loaded.info.input_attrs = std::move(input_attrs);
    loaded.info.output_attrs = std::move(output_attrs);

    models_[model_path] = std::move(loaded);
    LOG_INFO("Model loaded successfully: {}", model_path);

    return true;
}

rknn_context ModelManager::create_worker_context(const std::string& model_path, int core_mask) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = models_.find(model_path);
    if (it == models_.end()) {
        LOG_ERROR("Cannot create worker context: model not loaded: {}", model_path);
        return 0;
    }

    rknn_context dup_ctx = 0;
    int ret = rknn_dup_context(&it->second.master_ctx, &dup_ctx);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("rknn_dup_context failed for {}: ret={}", model_path, ret);
        return 0;
    }

    // 绑定 NPU 核心
    if (core_mask != NpuCoreMask::AUTO) {
        ret = rknn_set_core_mask(dup_ctx, static_cast<rknn_core_mask>(core_mask));
        if (ret != RKNN_SUCC) {
            LOG_WARN("rknn_set_core_mask({}) failed: ret={}, using AUTO", core_mask, ret);
            // 不致命, 继续使用 AUTO 调度
        } else {
            LOG_DEBUG("Worker context bound to NPU core mask={}", core_mask);
        }
    }

    return dup_ctx;
}

void ModelManager::release_worker_context(rknn_context ctx) {
    if (ctx != 0) {
        rknn_destroy(ctx);
    }
}

const ModelInfo* ModelManager::get_model_info(const std::string& model_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(model_path);
    if (it == models_.end()) return nullptr;
    return &it->second.info;
}

bool ModelManager::is_loaded(const std::string& model_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.count(model_path) > 0;
}

void ModelManager::unload_model(const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(model_path);
    if (it == models_.end()) return;

    LOG_INFO("Unloading model: {}", model_path);
    rknn_destroy(it->second.master_ctx);
    models_.erase(it);
}

void ModelManager::unload_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [path, loaded] : models_) {
        LOG_INFO("Unloading model: {}", path);
        rknn_destroy(loaded.master_ctx);
    }
    models_.clear();
}

size_t ModelManager::loaded_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.size();
}

} // namespace infer_server

#endif // HAS_RKNN
