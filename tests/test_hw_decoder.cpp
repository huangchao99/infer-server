/**
 * @file test_hw_decoder.cpp
 * @brief FFmpeg + RKMPP 硬件解码器测试
 *
 * 需要在 ARM 设备上运行, 可能需要 root 权限 (RKMPP 需要访问 /dev/mpp)。
 *
 * 用法: sudo ./test_hw_decoder <rtsp_url> [num_frames]
 * 示例: sudo ./test_hw_decoder "rtsp://admin:pass@192.168.254.124:554/Streaming/Channels/102" 50
 *
 * 测试内容:
 *   1. 连接 RTSP 流
 *   2. 初始化 RKMPP 硬件解码器
 *   3. 解码 N 帧并验证 NV12 数据有效性
 *   4. 测量解码 FPS
 *   5. 资源正确释放
 */

#ifdef HAS_FFMPEG

#include "infer_server/decoder/hw_decoder.h"
#include "infer_server/common/logger.h"

#include <iostream>
#include <chrono>
#include <string>
#include <numeric>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rtsp_url> [num_frames=100]" << std::endl;
        std::cerr << "Example: sudo " << argv[0]
                  << " \"rtsp://admin:pass@192.168.254.124:554/Streaming/Channels/102\" 50"
                  << std::endl;
        return 1;
    }

    std::string rtsp_url = argv[1];
    int num_frames = (argc > 2) ? std::atoi(argv[2]) : 100;

    infer_server::logger::init("debug");

    std::cout << "========================================" << std::endl;
    std::cout << "  HwDecoder Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "RTSP URL: " << rtsp_url << std::endl;
    std::cout << "Target frames: " << num_frames << std::endl;
    std::cout << std::endl;

    // ========================
    // 1. 打开解码器
    // ========================
    std::cout << "[TEST] Opening decoder..." << std::endl;

    infer_server::HwDecoder decoder;
    infer_server::HwDecoder::Config config;
    config.rtsp_url = rtsp_url;
    config.tcp_transport = true;
    config.connect_timeout_sec = 10;
    config.read_timeout_sec = 5;

    if (!decoder.open(config)) {
        std::cout << "[FAIL] Failed to open decoder" << std::endl;
        return 1;
    }

    std::cout << "[PASS] Decoder opened: "
              << decoder.get_width() << "x" << decoder.get_height()
              << " @ " << decoder.get_fps() << " fps"
              << ", codec=" << decoder.get_codec_name()
              << ", hw=" << (decoder.is_hardware() ? "yes" : "no")
              << std::endl;

    // ========================
    // 2. 解码帧
    // ========================
    std::cout << std::endl;
    std::cout << "[TEST] Decoding " << num_frames << " frames..." << std::endl;

    int decoded_count = 0;
    int error_count = 0;
    size_t total_bytes = 0;
    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < num_frames; i++) {
        auto frame = decoder.decode_frame();
        if (!frame) {
            std::cout << "[WARN] decode_frame returned nullopt at frame " << i << std::endl;
            error_count++;
            if (error_count > 5) {
                std::cout << "[FAIL] Too many errors, stopping" << std::endl;
                break;
            }
            continue;
        }

        decoded_count++;
        total_bytes += frame->nv12_data->size();

        // 验证 NV12 数据
        int expected_size = frame->width * frame->height * 3 / 2;
        bool size_ok = (static_cast<int>(frame->nv12_data->size()) == expected_size);
        bool data_ok = false;

        // 检查数据不全为零 (至少 Y 通道有数据)
        for (size_t j = 0; j < std::min<size_t>(1000, frame->nv12_data->size()); j++) {
            if ((*frame->nv12_data)[j] != 0) {
                data_ok = true;
                break;
            }
        }

        if (i < 3 || i == num_frames - 1 || !size_ok || !data_ok) {
            std::cout << "  Frame " << i << ": "
                      << frame->width << "x" << frame->height
                      << ", pts=" << frame->pts
                      << ", ts=" << frame->timestamp_ms
                      << ", size=" << frame->nv12_data->size()
                      << (size_ok ? " [size OK]" : " [size MISMATCH]")
                      << (data_ok ? " [data OK]" : " [data ZERO!]")
                      << std::endl;
        }

        if (!size_ok || !data_ok) {
            error_count++;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
    double actual_fps = decoded_count / elapsed_sec;

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Decoded:     " << decoded_count << " / " << num_frames << std::endl;
    std::cout << "  Errors:      " << error_count << std::endl;
    std::cout << "  Time:        " << elapsed_sec << "s" << std::endl;
    std::cout << "  FPS:         " << actual_fps << std::endl;
    std::cout << "  Total data:  " << (total_bytes / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "  Avg frame:   " << (decoded_count > 0 ? total_bytes / decoded_count : 0) << " bytes" << std::endl;

    // ========================
    // 3. 关闭并验证
    // ========================
    decoder.close();
    std::cout << std::endl;
    std::cout << (error_count == 0 ? "[PASS]" : "[FAIL]")
              << " HwDecoder test "
              << (error_count == 0 ? "passed" : "failed")
              << std::endl;

    infer_server::logger::shutdown();
    return error_count > 0 ? 1 : 0;
}

#else // !HAS_FFMPEG

#include <iostream>
int main() {
    std::cerr << "This test requires HAS_FFMPEG. "
              << "Build with -DENABLE_FFMPEG=ON" << std::endl;
    return 1;
}

#endif
