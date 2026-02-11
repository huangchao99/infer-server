/**
 * @file test_decode_pipeline.cpp
 * @brief 解码流水线集成测试: RTSP → 解码 → RGA → JPEG → 缓存
 *
 * 完整测试 Phase 2 的数据流水线, 模拟实际的解码线程行为:
 * 1. 连接 RTSP 流, RKMPP 硬件解码
 * 2. frame_skip 跳帧
 * 3. RGA 缩放 NV12→RGB (模拟推理输入)
 * 4. RGA 缩放 NV12→RGB (缓存尺寸) → JPEG 编码 → 写入 ImageCache
 * 5. 输出统计信息, 保存样本 JPEG 到磁盘
 *
 * 用法: sudo ./test_decode_pipeline <rtsp_url> [duration_sec=10] [output_dir=.]
 */

#if defined(HAS_FFMPEG) && defined(HAS_RGA) && defined(HAS_TURBOJPEG)

#include "infer_server/decoder/hw_decoder.h"
#include "infer_server/processor/rga_processor.h"
#include "infer_server/cache/jpeg_encoder.h"
#include "infer_server/cache/image_cache.h"
#include "infer_server/common/logger.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rtsp_url> [duration_sec=10] [output_dir=.]"
                  << std::endl;
        return 1;
    }

    std::string rtsp_url = argv[1];
    int duration_sec = (argc > 2) ? std::atoi(argv[2]) : 10;
    std::string output_dir = (argc > 3) ? argv[3] : ".";

    infer_server::logger::init("info");
    fs::create_directories(output_dir);

    std::cout << "========================================" << std::endl;
    std::cout << "  Decode Pipeline Integration Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "RTSP URL:     " << rtsp_url << std::endl;
    std::cout << "Duration:     " << duration_sec << "s" << std::endl;
    std::cout << "Output dir:   " << output_dir << std::endl;
    std::cout << std::endl;

    // ========================
    // 初始化组件
    // ========================
    const std::string cam_id = "test_cam";
    const int frame_skip = 5;
    const int model_w = 640, model_h = 640;  // 模拟模型输入尺寸

    // 解码器
    infer_server::HwDecoder decoder;
    infer_server::HwDecoder::Config dec_config;
    dec_config.rtsp_url = rtsp_url;
    dec_config.tcp_transport = true;

    if (!decoder.open(dec_config)) {
        std::cout << "[FAIL] Failed to open decoder" << std::endl;
        return 1;
    }
    std::cout << "[OK] Decoder: " << decoder.get_width() << "x" << decoder.get_height()
              << " @ " << decoder.get_fps() << "fps" << std::endl;

    // 计算缓存图片尺寸 (保持宽高比, 宽度 640)
    int cache_w = 640;
    int cache_h = infer_server::RgaProcessor::calc_proportional_height(
        decoder.get_width(), decoder.get_height(), cache_w);
    std::cout << "[OK] Cache size: " << cache_w << "x" << cache_h << std::endl;

    // JPEG 编码器
    infer_server::JpegEncoder jpeg_encoder;
    if (!jpeg_encoder.is_valid()) {
        std::cout << "[FAIL] JPEG encoder initialization failed" << std::endl;
        return 1;
    }
    std::cout << "[OK] JPEG encoder ready" << std::endl;

    // 图片缓存
    infer_server::ImageCache image_cache(5, 64);  // 5秒, 64MB
    std::cout << "[OK] Image cache ready (5s buffer, 64MB max)" << std::endl;

    std::cout << std::endl;

    // ========================
    // 模拟解码线程循环
    // ========================
    std::cout << "[RUN] Processing for " << duration_sec << " seconds..." << std::endl;
    std::cout << "      frame_skip=" << frame_skip
              << ", model_input=" << model_w << "x" << model_h
              << ", cache_size=" << cache_w << "x" << cache_h
              << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    int total_decoded = 0;
    int total_processed = 0;
    int total_skipped = 0;
    size_t total_jpeg_bytes = 0;
    double total_rga_infer_ms = 0;
    double total_rga_cache_ms = 0;
    double total_jpeg_ms = 0;
    bool saved_sample = false;

    while (true) {
        // 检查是否超时
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();
        if (elapsed >= duration_sec) break;

        // 解码一帧
        auto frame = decoder.decode_frame();
        if (!frame) {
            std::cout << "[WARN] decode_frame returned nullopt after "
                      << total_decoded << " frames" << std::endl;
            break;
        }
        total_decoded++;

        // frame_skip 检查
        if (total_decoded % frame_skip != 0) {
            total_skipped++;
            continue;
        }

        // ---- 以下是需要处理的帧 ----

        // RGA 1: 推理输入 (NV12 → RGB, model size)
        auto t0 = std::chrono::steady_clock::now();
        auto infer_rgb = infer_server::RgaProcessor::nv12_to_rgb_resize(
            frame->nv12_data->data(), frame->width, frame->height,
            model_w, model_h);
        auto t1 = std::chrono::steady_clock::now();
        total_rga_infer_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!infer_rgb) {
            std::cout << "[WARN] RGA infer resize failed at frame " << total_decoded << std::endl;
            continue;
        }

        // RGA 2: 缓存图片 (NV12 → RGB, cache size)
        auto t2 = std::chrono::steady_clock::now();
        auto cache_rgb = infer_server::RgaProcessor::nv12_to_rgb_resize(
            frame->nv12_data->data(), frame->width, frame->height,
            cache_w, cache_h);
        auto t3 = std::chrono::steady_clock::now();
        total_rga_cache_ms += std::chrono::duration<double, std::milli>(t3 - t2).count();

        if (!cache_rgb) {
            std::cout << "[WARN] RGA cache resize failed at frame " << total_decoded << std::endl;
            continue;
        }

        // JPEG 编码
        auto t4 = std::chrono::steady_clock::now();
        auto jpeg = jpeg_encoder.encode(cache_rgb->data(), cache_w, cache_h, 75);
        auto t5 = std::chrono::steady_clock::now();
        total_jpeg_ms += std::chrono::duration<double, std::milli>(t5 - t4).count();

        if (jpeg.empty()) {
            std::cout << "[WARN] JPEG encode failed at frame " << total_decoded << std::endl;
            continue;
        }

        total_jpeg_bytes += jpeg.size();

        // 写入缓存
        infer_server::CachedFrame cached;
        cached.cam_id = cam_id;
        cached.frame_id = total_processed + 1;
        cached.timestamp_ms = frame->timestamp_ms;
        cached.width = cache_w;
        cached.height = cache_h;
        cached.jpeg_data = std::make_shared<std::vector<uint8_t>>(std::move(jpeg));
        image_cache.add_frame(std::move(cached));

        total_processed++;

        // 保存第一帧作为样本
        if (!saved_sample) {
            std::string jpeg_path = output_dir + "/pipeline_sample.jpg";
            auto latest = image_cache.get_latest_frame(cam_id);
            if (latest && latest->jpeg_data) {
                std::ofstream ofs(jpeg_path, std::ios::binary);
                ofs.write(reinterpret_cast<const char*>(latest->jpeg_data->data()),
                          latest->jpeg_data->size());
                std::cout << "  Saved sample JPEG: " << jpeg_path
                          << " (" << latest->jpeg_data->size() << " bytes)" << std::endl;
            }
            saved_sample = true;
        }

        // 进度报告
        if (total_processed % 20 == 0) {
            std::cout << "  Processed " << total_processed << " frames"
                      << " (decoded " << total_decoded << ")"
                      << " cache=" << image_cache.stream_frame_count(cam_id) << " frames"
                      << " mem=" << (image_cache.total_memory_bytes() / 1024) << "KB"
                      << std::endl;
        }
    }

    // ========================
    // 输出统计
    // ========================
    auto end_time = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Pipeline Statistics" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Duration:          " << total_sec << "s" << std::endl;
    std::cout << "  Total decoded:     " << total_decoded << std::endl;
    std::cout << "  Total processed:   " << total_processed << " (skip=" << frame_skip << ")" << std::endl;
    std::cout << "  Total skipped:     " << total_skipped << std::endl;
    std::cout << "  Decode FPS:        " << (total_decoded / total_sec) << std::endl;
    std::cout << "  Process FPS:       " << (total_processed / total_sec) << std::endl;
    std::cout << std::endl;

    if (total_processed > 0) {
        std::cout << "  Avg RGA infer:     " << (total_rga_infer_ms / total_processed) << "ms" << std::endl;
        std::cout << "  Avg RGA cache:     " << (total_rga_cache_ms / total_processed) << "ms" << std::endl;
        std::cout << "  Avg JPEG encode:   " << (total_jpeg_ms / total_processed) << "ms" << std::endl;
        std::cout << "  Avg JPEG size:     " << (total_jpeg_bytes / total_processed) << " bytes" << std::endl;
    }

    std::cout << std::endl;
    std::cout << "  Cache frames:      " << image_cache.stream_frame_count(cam_id) << std::endl;
    std::cout << "  Cache memory:      " << (image_cache.total_memory_bytes() / 1024) << "KB" << std::endl;

    // 测试缓存查询
    std::cout << std::endl;
    auto latest = image_cache.get_latest_frame(cam_id);
    if (latest) {
        std::cout << "[PASS] Cache latest frame: id=" << latest->frame_id
                  << " ts=" << latest->timestamp_ms
                  << " jpeg=" << latest->jpeg_size() << " bytes" << std::endl;
    } else {
        std::cout << "[FAIL] No frames in cache" << std::endl;
    }

    decoder.close();

    bool success = (total_processed > 0 && latest.has_value());
    std::cout << std::endl;
    std::cout << (success ? "[PASS]" : "[FAIL]")
              << " Decode pipeline test " << (success ? "passed" : "failed") << std::endl;

    infer_server::logger::shutdown();
    return success ? 0 : 1;
}

#else

#include <iostream>
int main() {
    std::cerr << "This test requires HAS_FFMPEG, HAS_RGA, and HAS_TURBOJPEG." << std::endl;
    std::cerr << "Build with -DENABLE_FFMPEG=ON -DENABLE_RGA=ON" << std::endl;
    return 1;
}

#endif
