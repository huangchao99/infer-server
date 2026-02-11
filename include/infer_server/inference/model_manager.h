#pragma once

/**
 * @file model_manager.h
 * @brief RKNN 模型管理器
 *
 * 负责:
 * - 模型文件加载 (rknn_init)
 * - 输入/输出 tensor 属性查询
 * - 为 InferWorker 创建独立的 rknn_context (rknn_dup_context)
 * - NPU 核心绑定 (rknn_set_core_mask)
 * - 模型卸载和资源释放
 *
 * 线程安全: 内部使用 mutex 保护, 可从多线程调用。
 */

#ifdef HAS_RKNN

#include "infer_server/inference/post_processor.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>

// RKNN API
#include "rknn_api.h"

namespace infer_server {

/// NPU 核心掩码定义 (与 rknn_api.h 中的定义一致)
struct NpuCoreMask {
    static constexpr int AUTO   = 0;    ///< RKNN_NPU_CORE_AUTO
    static constexpr int CORE_0 = 1;    ///< RKNN_NPU_CORE_0
    static constexpr int CORE_1 = 2;    ///< RKNN_NPU_CORE_1
    static constexpr int CORE_2 = 4;    ///< RKNN_NPU_CORE_2
    static constexpr int CORE_0_1 = 3;  ///< RKNN_NPU_CORE_0_1
    static constexpr int CORE_ALL = 7;  ///< RKNN_NPU_CORE_0_1_2

    /// 根据 worker ID 返回对应的核心掩码
    /// worker 0 -> Core0, 1 -> Core1, 2 -> Core2, >= 3 -> AUTO
    static int from_worker_id(int worker_id) {
        switch (worker_id) {
            case 0: return CORE_0;
            case 1: return CORE_1;
            case 2: return CORE_2;
            default: return AUTO;
        }
    }
};

/**
 * @brief 模型信息 (加载后查询到的属性)
 */
struct ModelInfo {
    std::string model_path;
    rknn_input_output_num io_num = {};

    /// 输入 tensor 属性列表
    std::vector<rknn_tensor_attr> input_attrs;

    /// 输出 tensor 属性列表
    std::vector<rknn_tensor_attr> output_attrs;

    /// 转换为 PostProcessor 使用的 TensorAttr 格式
    std::vector<TensorAttr> get_output_tensor_attrs() const;
};

/**
 * @brief RKNN 模型管理器
 */
class ModelManager {
public:
    ModelManager() = default;
    ~ModelManager();

    // 禁止拷贝
    ModelManager(const ModelManager&) = delete;
    ModelManager& operator=(const ModelManager&) = delete;

    /**
     * @brief 加载模型文件
     *
     * 读取 .rknn 模型文件, 调用 rknn_init, 查询输入输出属性。
     * 如果模型已加载, 直接返回 true。
     *
     * @param model_path 模型文件路径
     * @return true 加载成功
     */
    bool load_model(const std::string& model_path);

    /**
     * @brief 为 worker 线程创建独立的 rknn_context
     *
     * 使用 rknn_dup_context 从主 context 创建轻量级副本,
     * 并通过 rknn_set_core_mask 绑定到指定 NPU 核心。
     *
     * @param model_path 已加载的模型路径
     * @param core_mask  NPU 核心掩码 (参见 NpuCoreMask)
     * @return rknn_context, 失败返回 0
     */
    rknn_context create_worker_context(const std::string& model_path, int core_mask);

    /**
     * @brief 释放 worker context
     * @param ctx 要释放的 context
     */
    void release_worker_context(rknn_context ctx);

    /**
     * @brief 获取模型信息 (线程安全)
     * @param model_path 模型路径
     * @return 模型信息指针, 未找到返回 nullptr
     */
    const ModelInfo* get_model_info(const std::string& model_path) const;

    /**
     * @brief 检查模型是否已加载
     */
    bool is_loaded(const std::string& model_path) const;

    /**
     * @brief 卸载指定模型
     */
    void unload_model(const std::string& model_path);

    /**
     * @brief 卸载所有模型
     */
    void unload_all();

    /**
     * @brief 已加载的模型数量
     */
    size_t loaded_count() const;

private:
    struct LoadedModel {
        rknn_context master_ctx = 0;   ///< 主 context (用于 dup)
        ModelInfo info;
        std::vector<uint8_t> model_data; ///< 模型二进制数据 (dup_context 可能需要)
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, LoadedModel> models_;
};

} // namespace infer_server

#endif // HAS_RKNN
