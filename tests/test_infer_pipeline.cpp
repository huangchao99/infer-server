/**
 * @file test_infer_pipeline.cpp
 * @brief Phase 3 端到端推理流水线集成测试
 *
 * 完整流程: RTSP -> HwDecoder -> RGA -> InferenceEngine -> ZMQ
 *
 * 需要:
 * - ARM 设备 + root 权限
 * - RTSP 视频源
 * - RKNN 模型文件 (.rknn)
 * - FFmpeg-RK, RGA, RKNN, ZMQ 全部可用
 *
 * 用法:
 *   sudo ./test_infer_pipeline <rtsp_url> <model.rknn> [model_type] [num_frames]
 *
 * 示例:
 *   sudo ./test_infer_pipeline \
 *       "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102" \
 *       /path/to/yolov5.rknn \
 *       yolov5 \
 *       50
 */

#if defined(HAS_RKNN) && defined(HAS_FFMPEG) && defined(HAS_RGA)

#include "infer_server/inference/inference_engine.h"
#include "infer_server/inference/frame_result_collector.h"
#include "infer_server/decoder/hw_decoder.h"
#include "infer_server/processor/rga_processor.h"
#include "infer_server/common/config.h"
#include "infer_server/common/logger.h"
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

using namespace infer_server;

// ============================================================
// 全局统计
// ============================================================
static std::atomic<int> g_results_received{0};
static std::atomic<int> g_total_detections{0};
static std::mutex g_print_mutex;

static void on_result(const FrameResult& result) {
    g_results_received++;
    int n_dets = 0;
    for (auto& mr : result.results) {
        n_dets += static_cast<int>(mr.detections.size());
    }
    g_total_detections += n_dets;

    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << "  [Result] cam=" << result.cam_id
              << " frame=" << result.frame_id
              << " models=" << result.results.size()
              << " dets=" << n_dets;

    if (n_dets > 0) {
        std::cout << " -> ";
        for (auto& mr : result.results) {
            for (auto& d : mr.detections) {
                std::cout << d.class_name << "(" << d.confidence << ") ";
            }
        }
    }
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: sudo " << argv[0]
                  << " <rtsp_url> <model.rknn> [model_type] [num_frames]" << std::endl;
        std::cerr << "\nExamples:" << std::endl;
        std::cerr << "  sudo ./" << argv[0]
                  << " \"rtsp://admin:pass@192.168.1.100:554/stream\" model.rknn yolov5 50"
                  << std::endl;
        return 1;
    }

    std::string rtsp_url = argv[1];
    std::string model_path = argv[2];
    std::string model_type = (argc > 3) ? argv[3] : "yolov5";
    int num_frames = (argc > 4) ? std::atoi(argv[4]) : 30;

    logger::init("info");

    std::cout << "======================================" << std::endl;
    std::cout << "  Phase 3 Integration Pipeline Test" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "  RTSP:       " << rtsp_url << std::endl;
    std::cout << "  Model:      " << model_path << std::endl;
    std::cout << "  Type:       " << model_type << std::endl;
    std::cout << "  Frames:     " << num_frames << std::endl;
    std::cout << "======================================" << std::endl;

    // ========================
    // 1. 初始化推理引擎
    // ========================
    std::cout << "\n[Step 1] Initializing InferenceEngine..." << std::endl;

    ServerConfig config;
    config.num_infer_workers = 3;
    config.infer_queue_size = 18;
    config.zmq_endpoint = "ipc:///tmp/infer_server_test_pipeline.ipc";

    InferenceEngine engine(config);
    if (!engine.init()) {
        std::cerr << "Failed to init InferenceEngine" << std::endl;
        return 1;
    }

    // 设置结果回调
    engine.set_result_callback(on_result);

    // ========================
    // 2. 加载模型
    // ========================
    std::cout << "\n[Step 2] Loading model..." << std::endl;

    ModelConfig mc;
    mc.model_path = model_path;
    mc.task_name = "test_detection";
    mc.model_type = model_type;
    mc.input_width = 640;
    mc.input_height = 640;
    mc.conf_threshold = 0.25f;
    mc.nms_threshold = 0.45f;

    if (!engine.load_models({mc})) {
        std::cerr << "Failed to load model" << std::endl;
        return 1;
    }

    // 获取模型信息
    const ModelInfo* model_info = engine.model_manager().get_model_info(model_path);
    if (!model_info) {
        std::cerr << "Failed to get model info" << std::endl;
        return 1;
    }

    // 从模型输入属性推断实际输入尺寸
    int model_input_w = mc.input_width;
    int model_input_h = mc.input_height;
    if (!model_info->input_attrs.empty()) {
        // NHWC: dims[1]=H, dims[2]=W
        auto& input_attr = model_info->input_attrs[0];
        if (input_attr.fmt == RKNN_TENSOR_NHWC) {
            model_input_h = input_attr.dims[1];
            model_input_w = input_attr.dims[2];
        } else {
            // NCHW: dims[2]=H, dims[3]=W
            model_input_h = input_attr.dims[2];
            model_input_w = input_attr.dims[3];
        }
        std::cout << "  Model input size: " << model_input_w << "x" << model_input_h << std::endl;
    }

    // ========================
    // 3. 打开 RTSP 流
    // ========================
    std::cout << "\n[Step 3] Opening RTSP stream..." << std::endl;

    HwDecoder decoder;
    HwDecoder::Config dec_cfg;
    dec_cfg.rtsp_url = rtsp_url;
    dec_cfg.tcp_transport = true;

    if (!decoder.open(dec_cfg)) {
        std::cerr << "Failed to open RTSP stream" << std::endl;
        return 1;
    }

    std::cout << "  Stream: " << decoder.width() << "x" << decoder.height()
              << " @ " << decoder.fps() << " fps"
              << " codec=" << decoder.codec_name()
              << " hw=" << (decoder.is_hardware() ? "yes" : "no") << std::endl;

    // ========================
    // 4. 解码 + RGA + 推理 循环
    // ========================
    std::cout << "\n[Step 4] Running decode->infer pipeline for "
              << num_frames << " frames..." << std::endl;

    auto t_start = std::chrono::steady_clock::now();
    int frames_decoded = 0;
    int frames_submitted = 0;
    int frame_skip = 5;  // 每 5 帧推理 1 次

    for (int i = 0; i < num_frames * frame_skip && frames_submitted < num_frames; i++) {
        auto frame = decoder.decode_frame();
        if (!frame) {
            std::cerr << "  Decode failed at frame " << i << std::endl;
            break;
        }
        frames_decoded++;

        // 跳帧
        if (i % frame_skip != 0) continue;

        // RGA: NV12 -> RGB resize 到模型输入尺寸
        auto rgb_data = RgaProcessor::nv12_to_rgb_resize(
            frame->nv12_data->data(),
            frame->width, frame->height,
            model_input_w, model_input_h
        );

        if (rgb_data.empty()) {
            std::cerr << "  RGA conversion failed at frame " << i << std::endl;
            continue;
        }

        // 构造 InferTask
        InferTask task;
        task.cam_id = "test_cam";
        task.rtsp_url = rtsp_url;
        task.frame_id = frame->frame_id;
        task.pts = frame->pts;
        task.timestamp_ms = frame->timestamp_ms;
        task.original_width = frame->width;
        task.original_height = frame->height;
        task.model_path = model_path;
        task.task_name = mc.task_name;
        task.model_type = mc.model_type;
        task.conf_threshold = mc.conf_threshold;
        task.nms_threshold = mc.nms_threshold;
        task.input_data = std::make_shared<std::vector<uint8_t>>(std::move(rgb_data));
        task.input_width = model_input_w;
        task.input_height = model_input_h;
        // 单模型, 不需要 aggregator

        engine.submit(std::move(task));
        frames_submitted++;
    }

    // 等待推理完成
    std::cout << "\n  Waiting for inference to complete..." << std::endl;
    int wait_count = 0;
    while (g_results_received.load() < frames_submitted && wait_count < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    auto t_end = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t_end - t_start).count();

    // ========================
    // 5. 统计报告
    // ========================
    std::cout << "\n======================================" << std::endl;
    std::cout << "  Pipeline Test Results" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "  Frames decoded:    " << frames_decoded << std::endl;
    std::cout << "  Frames submitted:  " << frames_submitted << std::endl;
    std::cout << "  Results received:  " << g_results_received.load() << std::endl;
    std::cout << "  Total detections:  " << g_total_detections.load() << std::endl;
    std::cout << "  Queue dropped:     " << engine.queue_dropped() << std::endl;
    std::cout << "  Total processed:   " << engine.total_processed() << std::endl;
    std::cout << "  Elapsed:           " << elapsed_sec << " s" << std::endl;
    if (elapsed_sec > 0) {
        std::cout << "  Decode FPS:        " << frames_decoded / elapsed_sec << std::endl;
        std::cout << "  Infer FPS:         " << frames_submitted / elapsed_sec << std::endl;
    }
#ifdef HAS_ZMQ
    std::cout << "  ZMQ published:     " << engine.zmq_published_count() << std::endl;
#endif
    std::cout << "======================================" << std::endl;

    // ========================
    // 6. 清理
    // ========================
    decoder.close();
    engine.shutdown();

    // 验证
    bool success = (g_results_received.load() > 0) && (frames_submitted > 0);
    std::cout << "\n" << (success ? "PASS" : "FAIL")
              << ": Pipeline integration test" << std::endl;

    logger::shutdown();
    return success ? 0 : 1;
}

#else // Missing dependencies

#include <iostream>
int main() {
    std::cout << "Phase 3 pipeline test requires all dependencies:" << std::endl;
    std::cout << "  HAS_RKNN, HAS_FFMPEG, HAS_RGA" << std::endl;
#ifndef HAS_RKNN
    std::cout << "  Missing: HAS_RKNN (ENABLE_RKNN=ON)" << std::endl;
#endif
#ifndef HAS_FFMPEG
    std::cout << "  Missing: HAS_FFMPEG (ENABLE_FFMPEG=ON)" << std::endl;
#endif
#ifndef HAS_RGA
    std::cout << "  Missing: HAS_RGA (ENABLE_RGA=ON)" << std::endl;
#endif
    return 0;
}

#endif
