/**
 * @file zmq_subscriber.cpp
 * @brief 独立 ZMQ 订阅者工具
 *
 * 用于手动验证推理服务器的 ZeroMQ 输出。
 * 连接到推理服务器的 PUB endpoint, 接收并打印 FrameResult JSON。
 *
 * 用法:
 *   ./zmq_subscriber [endpoint]
 *   ./zmq_subscriber                              # 默认 ipc:///tmp/infer_server.ipc
 *   ./zmq_subscriber tcp://127.0.0.1:5555
 *   ./zmq_subscriber ipc:///tmp/infer_server.ipc
 *
 * 输出格式:
 *   [序号] [摄像头ID] frame=帧号 ts=时间戳 results=模型数 detections=总检测数
 *   JSON (pretty-printed)
 */

#ifdef HAS_ZMQ

#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <chrono>
#include <iomanip>

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    std::string endpoint = "ipc:///tmp/infer_server.ipc";
    if (argc > 1) {
        endpoint = argv[1];
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "======================================" << std::endl;
    std::cout << "  ZMQ Subscriber Tool" << std::endl;
    std::cout << "  Endpoint: " << endpoint << std::endl;
    std::cout << "  Press Ctrl+C to stop" << std::endl;
    std::cout << "======================================" << std::endl;

    try {
        zmq::context_t ctx(1);
        zmq::socket_t sub(ctx, zmq::socket_type::sub);

        // 订阅所有消息
        sub.set(zmq::sockopt::subscribe, "");
        sub.set(zmq::sockopt::rcvtimeo, 1000);  // 1s 超时, 便于检查 stop 信号

        std::cout << "Connecting to " << endpoint << "..." << std::endl;
        sub.connect(endpoint);
        std::cout << "Connected. Waiting for messages..." << std::endl;

        uint64_t msg_count = 0;
        auto start_time = std::chrono::steady_clock::now();

        while (g_running) {
            zmq::message_t msg;
            auto result = sub.recv(msg, zmq::recv_flags::none);
            if (!result.has_value()) continue;  // 超时

            msg_count++;
            std::string json_str(static_cast<char*>(msg.data()), msg.size());

            try {
                auto j = nlohmann::json::parse(json_str);

                // 统计
                std::string cam_id = j.value("cam_id", "?");
                uint64_t frame_id = j.value("frame_id", 0u);
                int64_t ts = j.value("timestamp_ms", 0LL);
                int n_results = j.contains("results") ? static_cast<int>(j["results"].size()) : 0;

                int total_dets = 0;
                if (j.contains("results") && j["results"].is_array()) {
                    for (auto& r : j["results"]) {
                        if (r.contains("detections") && r["detections"].is_array()) {
                            total_dets += static_cast<int>(r["detections"].size());
                        }
                    }
                }

                // 摘要行
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                double elapsed_sec = std::chrono::duration<double>(elapsed).count();

                double fps = (elapsed_sec > 0) ? (msg_count / elapsed_sec) : 0.0;
                std::cout << "\n[" << msg_count << "] "
                          << "[" << cam_id << "] "
                          << "frame=" << frame_id << " "
                          << "ts=" << ts << " "
                          << "results=" << n_results << " "
                          << "detections=" << total_dets << " "
                          << "(" << std::fixed << std::setprecision(1) << fps << " msg/s)"
                          << std::endl;

                // 如果有检测结果, 打印详情
                if (total_dets > 0) {
                    for (auto& r : j["results"]) {
                        std::string task = r.value("task_name", "?");
                        double infer_ms = r.value("inference_time_ms", 0.0);
                        std::cout << "  [" << task << "] infer=" << std::fixed
                                  << std::setprecision(1) << infer_ms << "ms" << std::endl;
                        if (r.contains("detections")) {
                            for (auto& d : r["detections"]) {
                                std::cout << "    - " << d.value("class_name", "?")
                                          << " conf=" << std::setprecision(3)
                                          << d.value("confidence", 0.0f)
                                          << " box=[" << d["bbox"].value("x1", 0.0f)
                                          << "," << d["bbox"].value("y1", 0.0f)
                                          << "," << d["bbox"].value("x2", 0.0f)
                                          << "," << d["bbox"].value("y2", 0.0f) << "]"
                                          << std::endl;
                            }
                        }
                    }
                }

                // 每 100 条显示吞吐量
                if (msg_count % 100 == 0) {
                    double fps = msg_count / elapsed_sec;
                    std::cout << "\n--- " << msg_count << " messages received, "
                              << std::fixed << std::setprecision(1) << fps
                              << " msg/s ---" << std::endl;
                }

            } catch (const nlohmann::json::exception& e) {
                std::cerr << "[" << msg_count << "] JSON parse error: " << e.what() << std::endl;
                std::cerr << "  Raw: " << json_str.substr(0, 200) << "..." << std::endl;
            }
        }

        auto total_elapsed = std::chrono::steady_clock::now() - start_time;
        double total_sec = std::chrono::duration<double>(total_elapsed).count();

        std::cout << "\n======================================" << std::endl;
        std::cout << "  Total: " << msg_count << " messages in "
                  << std::fixed << std::setprecision(1) << total_sec << "s" << std::endl;
        if (total_sec > 0) {
            std::cout << "  Rate: " << std::setprecision(1)
                      << (msg_count / total_sec) << " msg/s" << std::endl;
        }
        std::cout << "======================================" << std::endl;

        sub.close();
        ctx.close();

    } catch (const zmq::error_t& e) {
        std::cerr << "ZMQ error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

#else // !HAS_ZMQ

#include <iostream>
int main() {
    std::cout << "ZeroMQ not available." << std::endl;
    std::cout << "Build with -DENABLE_ZMQ=ON and install libzmq3-dev." << std::endl;
    return 1;
}

#endif // HAS_ZMQ
