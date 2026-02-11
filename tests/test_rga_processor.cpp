/**
 * @file test_rga_processor.cpp
 * @brief RGA 硬件缩放 + 色彩转换测试
 *
 * 需要在 ARM 设备上运行, 可能需要 root 权限。
 * 从 RTSP 解码 1 帧, 用 RGA 缩放到多种尺寸, 保存为 PPM 文件供目视检查。
 *
 * 用法: sudo ./test_rga_processor <rtsp_url> [output_dir=.]
 * 示例: sudo ./test_rga_processor "rtsp://admin:pass@192.168.254.124:554/Streaming/Channels/102" ./output
 *
 * 测试内容:
 *   1. 解码一帧获取 NV12 数据
 *   2. RGA NV12→RGB + 缩放到 640x640
 *   3. RGA NV12→RGB + 缩放到 320x320
 *   4. RGA NV12→RGB + 缩放到 640x比例高度
 *   5. 保存 PPM 文件用于目视验证
 *   6. 测量每次 RGA 操作耗时
 */

#if defined(HAS_FFMPEG) && defined(HAS_RGA)

#include "infer_server/decoder/hw_decoder.h"
#include "infer_server/processor/rga_processor.h"
#include "infer_server/common/logger.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

/// 保存 RGB 数据为 PPM 文件 (简单无压缩图片格式, 可用任何图片查看器打开)
static bool save_ppm(const std::string& path, const uint8_t* rgb, int w, int h) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;

    ofs << "P6\n" << w << " " << h << "\n255\n";
    ofs.write(reinterpret_cast<const char*>(rgb), w * h * 3);
    return ofs.good();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rtsp_url> [output_dir=.]" << std::endl;
        return 1;
    }

    std::string rtsp_url = argv[1];
    std::string output_dir = (argc > 2) ? argv[2] : ".";

    infer_server::logger::init("info");
    fs::create_directories(output_dir);

    std::cout << "========================================" << std::endl;
    std::cout << "  RGA Processor Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "RTSP URL: " << rtsp_url << std::endl;
    std::cout << "Output dir: " << output_dir << std::endl;
    std::cout << std::endl;

    int pass_count = 0;
    int fail_count = 0;

    // ========================
    // 1. 解码一帧
    // ========================
    std::cout << "[TEST] Decoding one frame for RGA tests..." << std::endl;

    infer_server::HwDecoder decoder;
    infer_server::HwDecoder::Config config;
    config.rtsp_url = rtsp_url;
    config.tcp_transport = true;

    if (!decoder.open(config)) {
        std::cout << "[FAIL] Failed to open decoder" << std::endl;
        return 1;
    }

    auto frame = decoder.decode_frame();
    if (!frame) {
        std::cout << "[FAIL] Failed to decode frame" << std::endl;
        return 1;
    }

    std::cout << "[PASS] Decoded frame: " << frame->width << "x" << frame->height
              << ", NV12 size=" << frame->nv12_data->size() << " bytes" << std::endl;
    std::cout << std::endl;

    // ========================
    // 2. RGA 测试: NV12→RGB, 多种目标尺寸
    // ========================
    struct TestCase {
        std::string name;
        int dst_w, dst_h;
    };

    int prop_h = infer_server::RgaProcessor::calc_proportional_height(
        frame->width, frame->height, 640);

    std::vector<TestCase> tests = {
        {"640x640 (model input)", 640, 640},
        {"320x320 (small model)", 320, 320},
        {"640x" + std::to_string(prop_h) + " (proportional)", 640, prop_h},
    };

    for (const auto& tc : tests) {
        std::cout << "[TEST] RGA NV12(" << frame->width << "x" << frame->height
                  << ") -> RGB(" << tc.dst_w << "x" << tc.dst_h << ") ..."
                  << std::endl;

        auto t0 = std::chrono::steady_clock::now();

        auto rgb = infer_server::RgaProcessor::nv12_to_rgb_resize(
            frame->nv12_data->data(), frame->width, frame->height,
            tc.dst_w, tc.dst_h);

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!rgb) {
            std::cout << "[FAIL] " << tc.name << " - RGA failed" << std::endl;
            fail_count++;
            continue;
        }

        // 验证输出大小
        size_t expected = static_cast<size_t>(tc.dst_w) * tc.dst_h * 3;
        // RGA 可能对齐宽高到偶数
        int actual_w = (tc.dst_w + 1) & ~1;
        int actual_h = (tc.dst_h + 1) & ~1;
        size_t expected_aligned = static_cast<size_t>(actual_w) * actual_h * 3;

        if (rgb->size() != expected && rgb->size() != expected_aligned) {
            std::cout << "[FAIL] " << tc.name << " - size mismatch: got "
                      << rgb->size() << ", expected " << expected << std::endl;
            fail_count++;
            continue;
        }

        // 检查数据非零
        bool has_data = false;
        for (size_t i = 0; i < std::min<size_t>(1000, rgb->size()); i++) {
            if ((*rgb)[i] != 0) { has_data = true; break; }
        }
        if (!has_data) {
            std::cout << "[FAIL] " << tc.name << " - data is all zeros" << std::endl;
            fail_count++;
            continue;
        }

        // 保存 PPM
        std::string ppm_path = output_dir + "/rga_" +
            std::to_string(actual_w) + "x" + std::to_string(actual_h) + ".ppm";
        if (save_ppm(ppm_path, rgb->data(), actual_w, actual_h)) {
            std::cout << "       Saved: " << ppm_path << std::endl;
        }

        std::cout << "[PASS] " << tc.name
                  << " - " << ms << "ms, " << rgb->size() << " bytes"
                  << std::endl;
        pass_count++;
    }

    // ========================
    // 3. 清理
    // ========================
    decoder.close();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Results: " << pass_count << " passed, " << fail_count << " failed" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Check PPM files in " << output_dir << "/ for visual verification." << std::endl;
    std::cout << "You can view PPM files with: eog/feh/display <file.ppm>" << std::endl;

    infer_server::logger::shutdown();
    return fail_count > 0 ? 1 : 0;
}

#else // !HAS_FFMPEG || !HAS_RGA

#include <iostream>
int main() {
    std::cerr << "This test requires HAS_FFMPEG and HAS_RGA. "
              << "Build with -DENABLE_FFMPEG=ON -DENABLE_RGA=ON" << std::endl;
    return 1;
}

#endif
