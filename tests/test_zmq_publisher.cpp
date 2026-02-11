/**
 * @file test_zmq_publisher.cpp
 * @brief ZmqPublisher 单元测试
 *
 * 需要 libzmq (sudo apt install libzmq3-dev), 不需要 RKNN 硬件。
 * 使用 inproc:// 或 tcp://127.0.0.1 端点进行测试。
 *
 * 测试内容:
 * - 初始化/关闭
 * - PUB/SUB 消息收发
 * - JSON 格式验证
 * - 多消息发送
 * - 并发发送安全性
 */

#ifdef HAS_ZMQ

#include "infer_server/output/zmq_publisher.h"
#include "infer_server/common/logger.h"
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace infer_server;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CASE(name) \
    do { std::cout << "\n[TEST] " << name << std::endl; } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "  FAIL: " << #expr << " at line " << __LINE__ << std::endl; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a != _b) { \
            std::cerr << "  FAIL: " << #a << " == " << #b \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ << std::endl; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

#define PASS() \
    do { std::cout << "  PASS" << std::endl; g_tests_passed++; } while(0)

// ============================================================
// 测试 1: 基本初始化和关闭
// ============================================================
void test_init_shutdown() {
    TEST_CASE("Init and shutdown");

    ZmqPublisher pub("tcp://127.0.0.1:15555");
    ASSERT_TRUE(!pub.is_initialized());

    bool ok = pub.init();
    ASSERT_TRUE(ok);
    ASSERT_TRUE(pub.is_initialized());
    ASSERT_EQ(pub.published_count(), 0u);

    pub.shutdown();
    ASSERT_TRUE(!pub.is_initialized());

    PASS();
}

// ============================================================
// 测试 2: PUB/SUB 消息收发
// ============================================================
void test_pub_sub_message() {
    TEST_CASE("PUB/SUB message send and receive");

    // 使用 TCP 端点 (inproc 要求同一 context)
    std::string endpoint = "tcp://127.0.0.1:15556";

    ZmqPublisher pub(endpoint);
    bool ok = pub.init();
    ASSERT_TRUE(ok);

    // 创建 SUB socket
    zmq::context_t sub_ctx(1);
    zmq::socket_t sub_socket(sub_ctx, zmq::socket_type::sub);
    sub_socket.set(zmq::sockopt::subscribe, "");
    sub_socket.set(zmq::sockopt::rcvtimeo, 3000);  // 3s timeout
    sub_socket.connect(endpoint);

    // ZMQ PUB/SUB 需要时间建立连接 (slow joiner problem)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 发布一个 FrameResult
    FrameResult result;
    result.cam_id = "cam01";
    result.rtsp_url = "rtsp://example.com/stream";
    result.frame_id = 42;
    result.timestamp_ms = 1700000000000;
    result.original_width = 1920;
    result.original_height = 1080;

    Detection det;
    det.class_id = 0;
    det.class_name = "person";
    det.confidence = 0.95f;
    det.bbox = {100.0f, 200.0f, 300.0f, 400.0f};

    ModelResult mr;
    mr.task_name = "detection";
    mr.model_path = "/model/yolo.rknn";
    mr.inference_time_ms = 12.5;
    mr.detections.push_back(det);

    result.results.push_back(mr);

    pub.publish(result);

    // 接收消息
    zmq::message_t recv_msg;
    auto recv_result = sub_socket.recv(recv_msg, zmq::recv_flags::none);
    ASSERT_TRUE(recv_result.has_value());

    std::string json_str(static_cast<char*>(recv_msg.data()), recv_msg.size());
    std::cout << "  Received " << json_str.size() << " bytes" << std::endl;

    // 验证 JSON
    auto j = nlohmann::json::parse(json_str);
    ASSERT_TRUE(j["cam_id"] == "cam01");
    ASSERT_TRUE(j["rtsp_url"] == "rtsp://example.com/stream");
    ASSERT_EQ(j["frame_id"].get<uint64_t>(), 42u);
    ASSERT_EQ(j["original_width"].get<int>(), 1920);
    ASSERT_TRUE(j["results"].is_array());
    ASSERT_EQ(j["results"].size(), 1u);
    ASSERT_TRUE(j["results"][0]["task_name"] == "detection");
    ASSERT_EQ(j["results"][0]["detections"].size(), 1u);
    ASSERT_TRUE(j["results"][0]["detections"][0]["class_name"] == "person");

    std::cout << "  JSON parsed and validated OK" << std::endl;

    ASSERT_EQ(pub.published_count(), 1u);

    sub_socket.close();
    pub.shutdown();

    PASS();
}

// ============================================================
// 测试 3: 多消息发送
// ============================================================
void test_multiple_messages() {
    TEST_CASE("Multiple messages");

    std::string endpoint = "tcp://127.0.0.1:15557";

    ZmqPublisher pub(endpoint);
    pub.init();

    zmq::context_t sub_ctx(1);
    zmq::socket_t sub_socket(sub_ctx, zmq::socket_type::sub);
    sub_socket.set(zmq::sockopt::subscribe, "");
    sub_socket.set(zmq::sockopt::rcvtimeo, 2000);
    sub_socket.connect(endpoint);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const int NUM_MSGS = 10;
    for (int i = 0; i < NUM_MSGS; i++) {
        FrameResult result;
        result.cam_id = "cam" + std::to_string(i % 3);
        result.frame_id = i;
        pub.publish(result);
    }

    // 接收并计数
    int received = 0;
    for (int i = 0; i < NUM_MSGS; i++) {
        zmq::message_t msg;
        auto r = sub_socket.recv(msg, zmq::recv_flags::none);
        if (r.has_value()) {
            received++;
        } else {
            break;  // 超时
        }
    }

    std::cout << "  Sent " << NUM_MSGS << ", received " << received << std::endl;
    ASSERT_EQ(received, NUM_MSGS);
    ASSERT_EQ(pub.published_count(), static_cast<uint64_t>(NUM_MSGS));

    sub_socket.close();
    pub.shutdown();

    PASS();
}

// ============================================================
// 测试 4: 并发发送
// ============================================================
void test_concurrent_publish() {
    TEST_CASE("Concurrent publish (thread safety)");

    std::string endpoint = "tcp://127.0.0.1:15558";

    ZmqPublisher pub(endpoint);
    pub.init();

    zmq::context_t sub_ctx(1);
    zmq::socket_t sub_socket(sub_ctx, zmq::socket_type::sub);
    sub_socket.set(zmq::sockopt::subscribe, "");
    sub_socket.set(zmq::sockopt::rcvtimeo, 3000);
    sub_socket.connect(endpoint);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const int NUM_THREADS = 4;
    const int MSGS_PER_THREAD = 10;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&pub, t]() {
            for (int i = 0; i < MSGS_PER_THREAD; i++) {
                FrameResult result;
                result.cam_id = "cam_t" + std::to_string(t);
                result.frame_id = t * 1000 + i;
                pub.publish(result);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 接收
    int received = 0;
    int expected = NUM_THREADS * MSGS_PER_THREAD;
    for (int i = 0; i < expected; i++) {
        zmq::message_t msg;
        auto r = sub_socket.recv(msg, zmq::recv_flags::none);
        if (r.has_value()) {
            received++;
        } else {
            break;
        }
    }

    std::cout << "  Threads=" << NUM_THREADS << " msgs/thread=" << MSGS_PER_THREAD
              << " total_sent=" << pub.published_count()
              << " received=" << received << std::endl;

    ASSERT_EQ(pub.published_count(), static_cast<uint64_t>(expected));
    // 由于 PUB/SUB slow joiner, 可能丢一些消息, 但大部分应该收到
    ASSERT_TRUE(received >= expected * 0.8);

    sub_socket.close();
    pub.shutdown();

    PASS();
}

// ============================================================
// 测试 5: IPC endpoint
// ============================================================
void test_ipc_endpoint() {
    TEST_CASE("IPC endpoint (ipc://)");

    std::string endpoint = "ipc:///tmp/infer_server_test.ipc";

    ZmqPublisher pub(endpoint);
    bool ok = pub.init();
    ASSERT_TRUE(ok);
    ASSERT_TRUE(pub.endpoint() == endpoint);

    // 发一个消息 (即使没有订阅者, PUB 也不会阻塞)
    FrameResult result;
    result.cam_id = "test";
    result.frame_id = 1;
    pub.publish(result);

    ASSERT_EQ(pub.published_count(), 1u);
    pub.shutdown();

    PASS();
}

int main() {
    logger::init("warn");

    std::cout << "======================================" << std::endl;
    std::cout << "  ZmqPublisher Unit Tests" << std::endl;
    std::cout << "======================================" << std::endl;

    test_init_shutdown();
    test_pub_sub_message();
    test_multiple_messages();
    test_concurrent_publish();
    test_ipc_endpoint();

    std::cout << "\n======================================" << std::endl;
    std::cout << "  Results: " << g_tests_passed << " passed, "
              << g_tests_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    logger::shutdown();
    return g_tests_failed > 0 ? 1 : 0;
}

#else // !HAS_ZMQ

#include <iostream>
int main() {
    std::cout << "ZeroMQ not available, skipping ZmqPublisher tests." << std::endl;
    std::cout << "Build with -DENABLE_ZMQ=ON and install libzmq3-dev." << std::endl;
    return 0;
}

#endif // HAS_ZMQ
