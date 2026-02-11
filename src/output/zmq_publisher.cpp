/**
 * @file zmq_publisher.cpp
 * @brief ZeroMQ 推理结果发布器实现
 */

#ifdef HAS_ZMQ

#include "infer_server/output/zmq_publisher.h"
#include "infer_server/common/logger.h"
#include <zmq.hpp>
#include <nlohmann/json.hpp>

namespace infer_server {

ZmqPublisher::ZmqPublisher(const std::string& endpoint)
    : endpoint_(endpoint)
{
}

ZmqPublisher::~ZmqPublisher() {
    shutdown();
}

bool ZmqPublisher::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        LOG_WARN("ZmqPublisher already initialized");
        return true;
    }

    try {
        // 创建 ZMQ context (1 个 IO 线程)
        zmq_ctx_ = std::make_unique<zmq::context_t>(1);

        // 创建 PUB socket
        pub_socket_ = std::make_unique<zmq::socket_t>(*zmq_ctx_, zmq::socket_type::pub);

        // 设置 socket 选项
        // 发送高水位线: 当队列满时丢弃消息 (避免内存无限增长)
        int sndhwm = 100;
        pub_socket_->set(zmq::sockopt::sndhwm, sndhwm);

        // linger: socket 关闭时等待消息发送的时间 (毫秒)
        int linger = 1000;
        pub_socket_->set(zmq::sockopt::linger, linger);

        // 绑定
        pub_socket_->bind(endpoint_);

        initialized_ = true;
        LOG_INFO("ZmqPublisher initialized: {}", endpoint_);
        return true;

    } catch (const zmq::error_t& e) {
        LOG_ERROR("ZmqPublisher init failed: {} (endpoint={})", e.what(), endpoint_);
        pub_socket_.reset();
        zmq_ctx_.reset();
        return false;
    }
}

void ZmqPublisher::publish(const FrameResult& result) {
    if (!initialized_.load()) return;

    try {
        // JSON 序列化
        nlohmann::json j = result;
        std::string msg = j.dump();

        // 发送
        std::lock_guard<std::mutex> lock(mutex_);
        zmq::message_t zmq_msg(msg.data(), msg.size());
        auto send_result = pub_socket_->send(zmq_msg, zmq::send_flags::dontwait);

        if (send_result.has_value()) {
            published_count_.fetch_add(1, std::memory_order_relaxed);
            LOG_TRACE("ZMQ published: [{}] frame {} ({} bytes)",
                      result.cam_id, result.frame_id, msg.size());
        } else {
            LOG_WARN("ZMQ send returned no value (would block?)");
        }

    } catch (const zmq::error_t& e) {
        LOG_ERROR("ZMQ publish error: {}", e.what());
    } catch (const std::exception& e) {
        LOG_ERROR("ZMQ publish error (serialization): {}", e.what());
    }
}

void ZmqPublisher::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return;

    LOG_INFO("ZmqPublisher shutting down (published {} messages)", published_count_.load());

    try {
        if (pub_socket_) {
            pub_socket_->close();
            pub_socket_.reset();
        }
        if (zmq_ctx_) {
            zmq_ctx_->close();
            zmq_ctx_.reset();
        }
    } catch (const zmq::error_t& e) {
        LOG_ERROR("ZMQ shutdown error: {}", e.what());
    }

    initialized_ = false;
}

} // namespace infer_server

#endif // HAS_ZMQ
