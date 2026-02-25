#pragma once

/**
 * @file zmq_publisher.h
 * @brief ZeroMQ 推理结果发布器
 *
 * 使用 PUB socket 将 FrameResult 以 JSON 格式发布。
 * 下游程序 (行为分析/报警) 通过 SUB socket 订阅。
 *
 * 默认 endpoint: tcp://0.0.0.0:5555
 * 通信模式: PUB/SUB
 * 消息格式: JSON 字符串 (UTF-8)
 */

#ifdef HAS_ZMQ

#include "infer_server/common/types.h"
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

// 前向声明 zmq 类型
namespace zmq {
    class context_t;
    class socket_t;
}

namespace infer_server {

class ZmqPublisher {
public:
    /**
     * @brief 构造 ZMQ 发布器
     * @param endpoint ZMQ endpoint (如 "tcp://0.0.0.0:5555")
     */
    explicit ZmqPublisher(const std::string& endpoint = "tcp://0.0.0.0:5555");

    ~ZmqPublisher();

    // 禁止拷贝
    ZmqPublisher(const ZmqPublisher&) = delete;
    ZmqPublisher& operator=(const ZmqPublisher&) = delete;

    /**
     * @brief 初始化 ZMQ context 和 socket
     * @return true 初始化成功
     */
    bool init();

    /**
     * @brief 发布推理结果 (线程安全)
     *
     * 将 FrameResult 序列化为 JSON, 然后通过 ZMQ PUB socket 发送。
     * 如果没有订阅者, 消息会被静默丢弃 (PUB/SUB 语义)。
     *
     * @param result 完整的帧推理结果
     */
    void publish(const FrameResult& result);

    /**
     * @brief 关闭 ZMQ 连接
     */
    void shutdown();

    /**
     * @brief 是否已初始化
     */
    bool is_initialized() const { return initialized_.load(); }

    /**
     * @brief 已发布的消息计数
     */
    uint64_t published_count() const { return published_count_.load(); }

    /**
     * @brief 获取 endpoint
     */
    const std::string& endpoint() const { return endpoint_; }

private:
    std::string endpoint_;
    std::unique_ptr<zmq::context_t> zmq_ctx_;
    std::unique_ptr<zmq::socket_t> pub_socket_;
    std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> published_count_{0};
};

} // namespace infer_server

#endif // HAS_ZMQ
