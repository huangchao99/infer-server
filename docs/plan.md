RK3588 高性能推理服务器 (infer-server) 完整设计方案
一、系统架构总览
flowchart TB
    subgraph API ["REST API Layer (cpp-httplib)"]
        RestServer["REST Server :8080"]
    end

    subgraph Manager ["Stream Manager"]
        SM["StreamManager"]
        ConfigStore["ConfigPersistence\n(JSON file)"]
    end

    subgraph Decode ["Decode Layer (1 thread per stream)"]
        D1["DecoderWorker\ncam_01"]
        D2["DecoderWorker\ncam_02"]
        DN["DecoderWorker\ncam_N"]
    end

    subgraph RGA ["RGA Hardware Resize"]
        R1["RGA Processor"]
    end

    subgraph Queue ["Bounded Task Queue"]
        TQ["InferTaskQueue\n(bounded, drop oldest)"]
    end

    subgraph Infer ["NPU Inference Layer (3 threads, 1 per core)"]
        IW1["InferWorker\nNPU Core0"]
        IW2["InferWorker\nNPU Core1"]
        IW3["InferWorker\nNPU Core2"]
    end

    subgraph Post ["Post-Processing"]
        PP["PostProcessor\nYOLOv5/v8/v11"]
    end

    subgraph Agg ["Result Aggregation"]
        RA["ResultAggregator\n(collect multi-model per frame)"]
    end

    subgraph Output ["ZeroMQ Output"]
        ZMQ["ZmqPublisher\nipc:///tmp/infer_results"]
    end

    RestServer --> SM
    SM --> ConfigStore
    SM --> D1
    SM --> D2
    SM --> DN
    D1 --> R1
    D2 --> R1
    DN --> R1
    R1 --> TQ
    TQ --> IW1
    TQ --> IW2
    TQ --> IW3
    IW1 --> PP
    IW2 --> PP
    IW3 --> PP
    PP --> RA
    RA --> ZMQ
二、REST API 设计
基础路径: /api/v1
2.1 接口列表
- POST /api/v1/streams - 添加 RTSP 流（支持批量）
- DELETE /api/v1/streams/{cam_id} - 删除指定流
- GET /api/v1/streams - 列出所有流及其配置和运行状态
- GET /api/v1/streams/{cam_id} - 查看指定流的详细状态
- POST /api/v1/control/start-all - 启动所有已配置的流
- POST /api/v1/control/stop-all - 停止所有正在运行的流
- GET /api/v1/status - 系统总体健康状态和性能指标
2.2 添加流请求格式 (POST /api/v1/streams)
{
  "streams": [
    {
      "cam_id": "camera_01",
      "rtsp_url": "rtsp://admin:pass@192.168.254.124:554/Streaming/Channels/102",
      "frame_skip": 5,
      "models": [
        {
          "model_path": "/weights/yolo_phone.rknn",
          "task_name": "phone_detection",
          "model_type": "yolov5",
          "input_width": 640,
          "input_height": 640,
          "conf_threshold": 0.25,
          "nms_threshold": 0.45,
          "labels_file": "/weights/phone_labels.txt"
        },
        {
          "model_path": "/weights/yolo_smoking.rknn",
          "task_name": "smoking_detection",
          "model_type": "yolov8",
          "input_width": 640,
          "input_height": 640,
          "conf_threshold": 0.3,
          "nms_threshold": 0.45,
          "labels_file": "/weights/smoking_labels.txt"
        }
      ]
    }
  ]
}
字段说明：
- cam_id - 摄像头唯一标识，全局不可重复
- rtsp_url - RTSP 地址
- frame_skip - 每隔 N 帧推理一次（如 5 表示每 5 帧检测 1 帧，25fps 下为 5fps 检测）
- models[] - 该摄像头使用的模型列表（同一帧对多个模型并行推理）
  - model_path - RKNN 模型文件路径
  - task_name - 任务名称标识
  - model_type - 模型类型："yolov5" / "yolov8" / "yolov11"（决定后处理逻辑）
  - input_width, input_height - 模型输入尺寸
  - conf_threshold - 置信度阈值（默认 0.25）
  - nms_threshold - NMS 阈值（默认 0.45）
  - labels_file - 类别标签文件路径（可选，每行一个类别名）
添加成功响应：
{
  "code": 0,
  "message": "success",
  "data": {
    "added": ["camera_01"],
    "failed": []
  }
}
2.3 系统状态响应 (GET /api/v1/status)
{
  "code": 0,
  "uptime_seconds": 3600,
  "system": {
    "cpu_usage_percent": 45.2,
    "memory_used_mb": 1024,
    "memory_total_mb": 3800,
    "npu_cores_active": 3
  },
  "streams": {
    "total": 4,
    "running": 3,
    "error": 1
  },
  "inference": {
    "total_inferences": 12345,
    "avg_latency_ms": 38.5,
    "queue_size": 2,
    "queue_capacity": 16,
    "dropped_frames": 42
  },
  "models_loaded": ["yolo_phone.rknn", "yolo_smoking.rknn"]
}
2.4 流详情响应 (GET /api/v1/streams/{cam_id})
{
  "code": 0,
  "data": {
    "cam_id": "camera_01",
    "rtsp_url": "rtsp://...",
    "status": "running",
    "frame_skip": 5,
    "models": [...],
    "stats": {
      "decoded_frames": 5000,
      "inferred_frames": 1000,
      "dropped_frames": 5,
      "decode_fps": 25.0,
      "infer_fps": 4.8,
      "last_error": "",
      "uptime_seconds": 200,
      "reconnect_count": 0
    }
  }
}
三、ZeroMQ 输出格式
传输方式: ipc:///tmp/infer_server.ipc，使用 PUB/SUB 模式
每帧推理结果：
{
  "cam_id": "camera_01",
  "frame_id": 12345,
  "timestamp_ms": 1677654321123,
  "pts": 500000,
  "original_width": 640,
  "original_height": 360,
  "results": [
    {
      "task_name": "phone_detection",
      "model_path": "/weights/yolo_phone.rknn",
      "inference_time_ms": 35.2,
      "detections": [
        {
          "class_id": 0,
          "class_name": "phone",
          "confidence": 0.95,
          "bbox": {"x1": 100, "y1": 200, "x2": 300, "y2": 400}
        }
      ]
    },
    {
      "task_name": "smoking_detection",
      "model_path": "/weights/yolo_smoking.rknn",
      "inference_time_ms": 28.7,
      "detections": [
        {
          "class_id": 0,
          "class_name": "cigarette",
          "confidence": 0.88,
          "bbox": {"x1": 150, "y1": 250, "x2": 250, "y2": 350}
        }
      ]
    }
  ]
}
说明：
- bbox 坐标是相对于原始帧尺寸的（已从模型输入尺寸反算回来）
- frame_id 是该摄像头的全局递增帧号
- 同一帧的所有模型结果聚合后一起发送（保证原子性）
- 如果某个模型没有检测到目标，detections 为空数组
四、核心数据结构
// 推理任务（队列中的单元）
struct InferTask {
    std::string cam_id;
    uint64_t frame_id;
    int64_t pts;
    double timestamp_ms;
    int original_width;
    int original_height;
    
    // 模型信息
    std::string model_path;
    std::string task_name;
    std::string model_type;     // "yolov5", "yolov8", "yolov11"
    float conf_threshold;
    float nms_threshold;
    std::vector<std::string> labels;
    
    // 输入数据（RGA resize 后的 RGB 数据）
    std::shared_ptr<uint8_t[]> input_data;  // 或 DMA-BUF fd
    int input_width;
    int input_height;
    
    // 结果聚合器（同一帧多模型共享）
    std::shared_ptr<ResultAggregator> aggregator;
};
五、关键设计决策
5.1 解码流水线（尽量减少拷贝）
RTSP → FFmpeg(h264_rkmpp) → MppBuffer(NV12, DMA-BUF fd)
  → RGA(wrapbuffer_fd, resize+NV12→RGB) → DMA-BUF/内存
  → rknn_inputs_set(NHWC RGB)
- MPP 解码输出 NV12 格式的 MppBuffer（含 DMA-BUF fd）
- RGA 使用 importbuffer_fd 直接引用 DMA-BUF，硬件完成 resize + 色彩空间转换（NV12→RGB）
- RGA 输出到预分配的缓冲区，直接喂给 RKNN
- 整个流程仅在 RGA→RKNN 之间可能有一次内存拷贝（如果无法共享 DMA-BUF）
5.2 有界队列与丢帧策略
- 每个解码器输出队列：容量 2（保持最新帧，丢弃旧帧）
- 全局推理任务队列：容量 = 3 x NPU核心数 x 2 = 18
- 丢帧策略：队列满时丢弃最旧的帧（保证实时性）
- 所有丢帧计入统计，可通过 status API 查看
5.3 模型管理
- ModelManager 使用 LRU 缓存，相同 model_path 只加载一次
- 多流使用同一模型时，通过 rknn_dup_context 共享模型权重
- 每个 NPU 核心绑定独立的 rknn_context（线程安全）
5.4 RTSP 断线重连
- 解码线程检测到连接断开后，指数退避重连（1s, 2s, 4s... 最大 30s）
- 重连期间状态标记为 "reconnecting"
- 重连次数计入统计
5.5 持久化与自动恢复
- 每次添加/删除流时，将完整配置序列化到 /etc/infer-server/streams.json
- 程序启动时自动加载该文件，恢复所有之前运行的流
5.6 同流多模型并行
- 解码器每产生一帧，为每个模型创建独立的 InferTask
- InferTask 共享一个 ResultAggregator（原子计数器追踪完成数）
- 不同 InferTask 可被不同 NPU 核心并行处理
- 全部模型完成后，ResultAggregator 触发 ZMQ 发布
六、项目结构
infer-server/
├── CMakeLists.txt              # 主构建文件
├── cmake/
│   ├── FindRKNN.cmake          # RKNN 库查找
│   ├── FindRGA.cmake           # RGA 库查找
│   └── FindFFmpegRK.cmake      # FFmpeg-RK 库查找
├── third_party/                # 第三方头文件
│   ├── httplib.h
│   ├── json.hpp
│   └── zmq.hpp + zmq_addon.hpp
├── include/infer_server/
│   ├── common/
│   │   ├── types.h             # 核心数据类型
│   │   ├── config.h            # 配置结构和序列化
│   │   ├── bounded_queue.h     # 线程安全有界队列
│   │   └── logger.h            # spdlog 封装
│   ├── decoder/
│   │   └── hw_decoder.h        # FFmpeg+RKMPP 硬件解码
│   ├── processor/
│   │   └── rga_processor.h     # RGA 硬件缩放/色彩转换
│   ├── inference/
│   │   ├── model_manager.h     # RKNN 模型加载/缓存
│   │   ├── infer_worker.h      # NPU 推理工作线程
│   │   └── post_process.h      # YOLO 后处理
│   ├── output/
│   │   ├── result_aggregator.h # 多模型结果聚合
│   │   └── zmq_publisher.h     # ZeroMQ 发布者
│   ├── api/
│   │   └── rest_server.h       # REST API 服务
│   └── pipeline/
│       └── stream_manager.h    # 流生命周期管理
├── src/                        # 对应实现文件
│   ├── main.cpp
│   ├── common/ decoder/ processor/ inference/ output/ api/ pipeline/
├── tests/
│   ├── CMakeLists.txt
│   ├── test_bounded_queue.cpp  # 阶段1: 队列单测
│   ├── test_hw_decoder.cpp     # 阶段2: 硬件解码测试
│   ├── test_rga_processor.cpp  # 阶段3: RGA 测试
│   ├── test_rknn_inference.cpp # 阶段4: RKNN 推理测试
│   ├── test_post_process.cpp   # 阶段5: 后处理测试
│   ├── test_zmq_output.cpp     # 阶段6: ZMQ 输出测试
│   ├── test_rest_api.cpp       # 阶段7: REST API 测试
│   ├── test_pipeline.cpp       # 阶段8: 全流程集成测试
│   └── tools/
│       ├── zmq_subscriber.cpp  # ZMQ 订阅者（接收结果的测试工具）
│       └── fake_rtsp.sh        # 用 FFmpeg 推送测试 RTSP 流
└── docs/
    └── testing_guide.md        # 详细的分步测试文档
七、第三方依赖
暂时无法在飞书文档外展示此内容
八、测试策略（分步验证）
每个阶段都有独立的测试程序，可以单独编译运行，确保每个组件正确后再集成：
- 阶段 1 - BoundedQueue: 纯 CPU 测试，多线程生产消费，验证有界和丢帧
- 阶段 2 - HwDecoder: 连接真实 RTSP 或本地视频，验证 RKMPP 解码输出帧
- 阶段 3 - RGA: 对解码帧做 resize + NV12→RGB，保存结果为 PPM 图片验证
- 阶段 4 - RKNN: 加载 .rknn 模型，输入测试图片，验证推理输出
- 阶段 5 - PostProcess: 用已知的模型输出数据，验证 YOLO 后处理输出检测框
- 阶段 6 - ZMQ: 启动 publisher + subscriber，验证 JSON 消息正确传输
- 阶段 7 - REST API: 用 curl 测试所有端点，验证增删查改
- 阶段 8 - 全流程: RTSP→解码→推理→ZMQ输出，端到端验证
九、实现分期
由于项目较大，建议分 4 期实现：
第 1 期：基础设施（CMake + 公共组件 + 有界队列 + 日志 + 配置）
第 2 期：解码 + 缩放（FFmpeg RKMPP 解码器 + RGA 处理器 + 帧管理）
第 3 期：推理 + 后处理 + 输出（ModelManager + InferWorker + PostProcess + ZMQ）
第 4 期：API + 管理 + 集成（REST Server + StreamManager + 持久化 + 集成测试）


---

Phase 2: 解码流水线 + 图片缓存
一、数据流概览
flowchart TB
    RTSP["RTSP Stream\n25fps H.264"]
    HwDec["HwDecoder\nFFmpeg + h264_rkmpp"]
    SkipCheck{"frame_count\n% skip == 0?"}
    RGA_Infer["RgaProcessor\nNV12 -> RGB\nresize to model_size"]
    RGA_Cache["RgaProcessor\nNV12 -> RGB\nresize to cache_size"]
    JpegEnc["JpegEncoder\nRGB -> JPEG"]
    InferQ["BoundedQueue\nInferTask"]
    Cache["ImageCache\nRing Buffer per stream\nlast 5 seconds"]
    Discard["Discard Frame"]

    RTSP --> HwDec
    HwDec --> SkipCheck
    SkipCheck -->|Yes| RGA_Infer
    SkipCheck -->|Yes| RGA_Cache
    SkipCheck -->|No| Discard
    RGA_Infer --> InferQ
    RGA_Cache --> JpegEnc
    JpegEnc --> Cache
对于每一帧通过 frame_skip 检查的帧，在解码线程中同步执行两次 RGA 操作（硬件加速，每次 < 1ms），然后将 RGB 数据分别送入推理队列和 JPEG 缓存。AVFrame 在两次 RGA 完成后立即释放，归还解码器缓冲池。
二、类型修改 (types.h)
2.1 FrameResult 添加 rtsp_url 字段
struct FrameResult {
    std::string cam_id;
    std::string rtsp_url;    // <-- 新增
    uint64_t frame_id = 0;
    // ... 其余不变
};
2.2 新增 DecodedFrame（内部传递类型）
struct DecodedFrame {
    std::string cam_id;
    std::string rtsp_url;
    uint64_t frame_id = 0;
    int64_t pts = 0;
    int64_t timestamp_ms = 0;
    int width = 0;
    int height = 0;
    // AVFrame 数据的封装 (NV12 格式)
    // data[0] = Y plane, data[1] = UV interleaved
    std::shared_ptr<uint8_t[]> y_data;
    std::shared_ptr<uint8_t[]> uv_data;
    int y_linesize = 0;
    int uv_linesize = 0;
    size_t total_size = 0;  // Y + UV 总字节数
};
2.3 InferTask 添加 rtsp_url
struct InferTask {
    std::string rtsp_url;  // <-- 新增
    // ... 其余不变
};
三、新增配置参数 (config.h)
struct ServerConfig {
    // ... 原有字段 ...

    // 图片缓存配置
    int cache_duration_sec = 5;       // Ring Buffer 保留时长(秒)
    int cache_jpeg_quality = 75;      // JPEG 压缩质量 (1-100)
    int cache_resize_width = 640;     // 缓存图片宽度 (0=保持原始宽度)
    int cache_resize_height = 0;      // 缓存图片高度 (0=按比例计算)
    int cache_max_memory_mb = 64;     // 缓存最大内存 (MB)
};
四、新增组件
4.1 HwDecoder (include/infer_server/decoder/hw_decoder.h)
硬件解码器，封装 FFmpeg + h264_rkmpp：
- open(rtsp_url) - 连接 RTSP 流，初始化 RKMPP 解码器
- decode_frame() -> std::optional<DecodedFrame> - 解码一帧，返回 NV12 数据
- close() - 释放资源
- get_width/height/fps() - 获取流参数
- 内部使用 TCP 传输（-rtsp_transport tcp）
- 连接超时 5 秒，读取超时 5 秒
- 内部管理 AVFormatContext、AVCodecContext 生命周期
- NV12 数据从 AVFrame 拷贝到独立 buffer（释放 AVFrame 归还解码器缓冲池）
关键 FFmpeg 调用链：
avformat_open_input (rtsp, tcp)
  -> avformat_find_stream_info
  -> avcodec_find_decoder_by_name("h264_rkmpp")
  -> avcodec_open2
  -> av_read_frame + avcodec_send_packet + avcodec_receive_frame
  -> 提取 NV12 数据 (frame->data[0], frame->data[1])
  -> av_frame_unref
FFmpeg 安装路径: /opt/ffmpeg-rk（头文件 /opt/ffmpeg-rk/include，库 /opt/ffmpeg-rk/lib）
4.2 RgaProcessor (include/infer_server/processor/rga_processor.h)
RGA 硬件缩放和色彩转换：
- resize_nv12_to_rgb(nv12_y, nv12_uv, src_w, src_h, rgb_out, dst_w, dst_h) - NV12 -> RGB + resize
- resize_nv12(nv12_y, nv12_uv, src_w, src_h, nv12_out, dst_w, dst_h) - NV12 -> NV12 resize
- 使用 RGA 的 imresize / imcvtcolor API
- 头文件路径: /usr/include/rga/（rga.h, drmrga.h, 以及可能的 im2d.hpp）
- 库: librga.so
4.3 JpegEncoder (include/infer_server/cache/jpeg_encoder.h)
JPEG 编码工具（基于 libjpeg-turbo）：
- encode(rgb_data, width, height, quality) -> std::vector<uint8_t> (JPEG 数据)
- 使用 TurboJPEG API（tjInitCompress, tjCompress2）
- 依赖: sudo apt install libjpeg-turbo8-dev
4.4 ImageCache (include/infer_server/cache/image_cache.h)
每个流维护一个 Ring Buffer，缓存最近 N 秒的 JPEG 帧：
struct CachedFrame {
    std::string cam_id;
    uint64_t frame_id;
    int64_t timestamp_ms;
    int width, height;
    std::shared_ptr<std::vector<uint8_t>> jpeg_data;
};

class ImageCache {
    void add_stream(const std::string& cam_id);
    void remove_stream(const std::string& cam_id);
    void add_frame(CachedFrame frame);
    // 精确匹配
    std::optional<CachedFrame> get_frame(const std::string& cam_id, int64_t ts_ms);
    // 最近匹配
    std::optional<CachedFrame> get_nearest_frame(const std::string& cam_id, int64_t ts_ms);
    // 获取某流最新帧
    std::optional<CachedFrame> get_latest_frame(const std::string& cam_id);
    // 内存统计
    size_t total_memory_bytes() const;
};
- 每个流一个 std::deque<CachedFrame>，受独立 mutex 保护
- add_frame 时自动清理过期帧（超过 cache_duration_sec）
- 总内存超过 cache_max_memory_mb 时从最旧的流开始淘汰
- 线程安全（多个解码线程同时写入不同流，API 线程读取）
五、CMake 变更 (CMakeLists.txt)
- 将 ENABLE_FFMPEG、ENABLE_RGA 默认改为 ON
- 新增 cmake/FindFFmpegRK.cmake（搜索 /opt/ffmpeg-rk）
- 新增 cmake/FindRGA.cmake（搜索 /usr/include/rga/）
- 新增 cmake/FindTurboJPEG.cmake
- 将 Phase 2 源码编入 infer_server_core
- 更新 tests/CMakeLists.txt 添加新测试目标
系统依赖（需要在 ARM 设备上安装）:
- sudo apt install libjpeg-turbo8-dev
- FFmpeg-RK 已在 /opt/ffmpeg-rk
- librga 已在 /usr/include/rga/ + /usr/lib/
六、新增文件清单
cmake/
  FindFFmpegRK.cmake              # FFmpeg at /opt/ffmpeg-rk
  FindRGA.cmake                   # librga
  FindTurboJPEG.cmake             # libjpeg-turbo

include/infer_server/
  decoder/hw_decoder.h            # 硬件解码器
  processor/rga_processor.h       # RGA 处理器
  cache/jpeg_encoder.h            # JPEG 编码
  cache/image_cache.h             # 图片缓存

src/
  decoder/hw_decoder.cpp
  processor/rga_processor.cpp
  cache/jpeg_encoder.cpp
  cache/image_cache.cpp

tests/
  test_hw_decoder.cpp             # 硬件解码测试
  test_rga_processor.cpp          # RGA 测试 (保存 PPM 验证)
  test_image_cache.cpp            # 缓存逻辑测试 (纯内存)
  test_decode_pipeline.cpp        # 集成: RTSP->解码->RGA->JPEG->缓存
七、测试策略
所有硬件相关测试需要在 ARM 设备上以 root 权限运行，RTSP URL 通过命令行参数传入。
- test_hw_decoder <rtsp_url> [frames=100] - 连接 RTSP，RKMPP 解码 N 帧，验证 NV12 数据有效，测量 FPS
- test_rga_processor <rtsp_url> [output_dir=./] - 解码 1 帧，RGA 缩放到多种尺寸，保存为 PPM 文件供目视检查
- test_image_cache (不需要硬件) - 用合成数据测试缓存的增删查、过期淘汰、内存限制
- test_decode_pipeline <rtsp_url> [duration_sec=10] - 完整流水线跑 N 秒，输出统计信息，保存样本 JPEG 到磁盘
测试 RTSP 地址参考:
rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102
rtsp://admin:hifleet321@192.168.254.123:554/Streaming/Channels/101





---

Phase 3: RKNN NPU 推理 + YOLO 后处理 + ZeroMQ 输出
整体架构
flowchart LR
    subgraph phase2 [Phase 2 已完成]
        Decoder[HwDecoder] --> RGA[RgaProcessor]
        RGA --> Cache[ImageCache]
    end

    subgraph phase3 [Phase 3 新增]
        RGA -->|"RGB data"| TaskQueue[BoundedQueue_InferTask]
        TaskQueue --> W0[InferWorker_0_NPU_Core0]
        TaskQueue --> W1[InferWorker_1_NPU_Core1]
        TaskQueue --> W2[InferWorker_2_NPU_Core2]
        W0 --> PostProc[PostProcessor]
        W1 --> PostProc
        W2 --> PostProc
        PostProc --> Collector[FrameResultCollector]
        Collector -->|"complete FrameResult"| ZMQ[ZmqPublisher]
    end

    ModelMgr[ModelManager] -.->|"rknn_context"| W0
    ModelMgr -.->|"rknn_context"| W1
    ModelMgr -.->|"rknn_context"| W2
    ZMQ -->|"ipc:///tmp/infer_server.ipc"| Downstream[下游行为分析程序]
数据流
1. Phase 2 的 RgaProcessor 将 NV12 转换为模型所需尺寸的 RGB 数据
2. 解码线程为每帧的每个模型创建 InferTask，同一帧的多模型任务共享同一个 FrameResultCollector
3. InferTask 被推入全局 BoundedQueue<InferTask>（容量=18）
4. 3 个 InferWorker 线程竞争消费任务，各自绑定一个 NPU 核心
5. Worker 调用 RKNN API 进行推理，然后通过 PostProcessor 做 YOLO 后处理
6. 结果通过 FrameResultCollector 聚合，当同一帧所有模型完成后输出完整 FrameResult
7. ZmqPublisher 通过 IPC 发布 JSON 格式的 FrameResult
新增/修改文件清单
CMake 模块
- 新建 cmake/FindRKNN.cmake -- 查找 RKNN 库
  - Header: /usr/local/include/rknn/rknn_api.h
  - Library: /usr/lib/librknnrt.so
- 新建 cmake/FindZMQ.cmake -- 查找 libzmq
  - Header: /usr/include/zmq.h
  - Library: /usr/lib/aarch64-linux-gnu/libzmq.so
- 修改 [CMakeLists.txt](CMakeLists.txt) -- 启用 RKNN/ZMQ 选项，添加条件编译，FetchContent 添加 cppzmq（header-only）
核心组件（6 个新文件）
1. ModelManager -- 模型加载与上下文管理
- 新建 include/infer_server/inference/model_manager.h
- 新建 src/inference/model_manager.cpp
class ModelManager {
public:
    struct ModelInfo {
        rknn_input_output_num io_num;
        std::vector<rknn_tensor_attr> input_attrs;
        std::vector<rknn_tensor_attr> output_attrs;
    };
    // 加载模型 (rknn_init, 查询 IO 属性)
    bool load_model(const std::string& model_path);
    // 为 worker 创建独立 context (rknn_dup_context + set_core_mask)
    rknn_context create_worker_context(const std::string& model_path, int core_mask);
    // 释放 worker context
    void release_worker_context(rknn_context ctx);
    const ModelInfo* get_model_info(const std::string& path) const;
    void unload_model(const std::string& path);
};
关键设计：
- 主 context 通过 rknn_init 加载一次，Worker 通过 rknn_dup_context 创建轻量级副本
- Worker 的 context 通过 rknn_set_core_mask 绑定到特定 NPU 核心
- 线程安全（std::mutex 保护内部 map）
2. PostProcessor -- YOLO 后处理
- 新建 include/infer_server/inference/post_processor.h
- 新建 src/inference/post_processor.cpp
class PostProcessor {
public:
    // YOLOv5: anchor-based, 3 输出头
    static std::vector<Detection> yolov5(
        float** outputs, const std::vector<rknn_tensor_attr>& attrs,
        int model_w, int model_h, int orig_w, int orig_h,
        float conf_thresh, float nms_thresh,
        const std::vector<std::string>& labels);
    // YOLOv8/v11: anchor-free, DFL box regression
    static std::vector<Detection> yolov8(/* 同上参数 */);
    // NMS 通用实现
    static void nms(std::vector<Detection>& dets, float threshold);
};
关键设计：
- 支持 want_float=1（float32 输出，简化流程）和 want_float=0（INT8 手动反量化，更高性能）
- YOLOv5: 3 个输出头，stride 8/16/32，预定义 anchor，grid 解码
- YOLOv8/v11: anchor-free，Distribution Focal Loss 解码
- 检测框坐标映射回原始帧尺寸（letterbox/等比例缩放）
- 此模块不依赖 RKNN 硬件，可在任何平台用合成数据测试
3. FrameResultCollector -- 多模型结果聚合
- 新建 include/infer_server/inference/frame_result_collector.h（header-only）
class FrameResultCollector {
public:
    FrameResultCollector(int total_models, FrameResult base_result);
    // 线程安全: 添加一个模型的结果
    // 当所有模型完成时返回完整的 FrameResult
    std::optional<FrameResult> add_result(ModelResult result);
};
关键设计：
- 解码线程为每帧创建一个 Collector，shared_ptr 存入 InferTask::aggregator
- 多个 InferWorker 可并发调用 add_result
- 使用 std::atomic<int> 计数 + std::mutex 保护结果数组
- 纯逻辑，不依赖任何硬件
4. InferWorker -- NPU 推理工作线程
- 新建 include/infer_server/inference/infer_worker.h
- 新建 src/inference/infer_worker.cpp
class InferWorker {
public:
    InferWorker(int worker_id, int core_mask,
                ModelManager& model_mgr,
                BoundedQueue<InferTask>& task_queue,
                std::function<void(FrameResult)> on_complete);
    void start();
    void stop();
private:
    void run(); // 主循环: pop task -> get/create ctx -> set input -> run -> get output -> post_process -> collect
    // 惰性创建: 首次遇到新 model_path 时调用 ModelManager::create_worker_context
    std::unordered_map<std::string, rknn_context> contexts_;
};
关键设计：
- 每个 Worker 独立线程，绑定一个 NPU 核心（Core0=1, Core1=2, Core2=4）
- 惰性创建 rknn_context：首次遇到某 model_path 时通过 rknn_dup_context 创建
- 推理流程: rknn_inputs_set -> rknn_run -> rknn_outputs_get -> PostProcessor -> Collector::add_result
- 当 Collector 返回完整结果时，调用 on_complete 回调（发送到 ZMQ）
5. ZmqPublisher -- ZeroMQ 结果发布
- 新建 include/infer_server/output/zmq_publisher.h
- 新建 src/output/zmq_publisher.cpp
class ZmqPublisher {
public:
    explicit ZmqPublisher(const std::string& endpoint);
    bool init();
    void publish(const FrameResult& result); // JSON 序列化 + zmq_send
    void shutdown();
};
关键设计：
- PUB socket，endpoint 默认 ipc:///tmp/infer_server.ipc
- FrameResult 序列化为 JSON 字符串后发送
- std::mutex 保护 socket（多个 Worker 可能并发调用 publish）
6. InferenceEngine -- 推理引擎统一管理
- 新建 include/infer_server/inference/inference_engine.h
- 新建 src/inference/inference_engine.cpp
class InferenceEngine {
public:
    InferenceEngine(const ServerConfig& config);
    bool init();
    // 预加载模型 (供 StreamManager 调用)
    bool load_models(const std::vector<ModelConfig>& models);
    // 提交推理任务
    bool submit(InferTask task);
    void shutdown();
    // 统计信息
    size_t queue_size() const;
    size_t queue_dropped() const;
private:
    ModelManager model_mgr_;
    BoundedQueue<InferTask> task_queue_;
    std::vector<std::unique_ptr<InferWorker>> workers_;
    ZmqPublisher zmq_pub_;
};
类型修改
- 修改 [include/infer_server/common/types.h](include/infer_server/common/types.h) -- InferTask::aggregator 类型改为 std::shared_ptr<FrameResultCollector>（或保持 void 但添加类型说明注释）
测试文件
无需硬件的测试（可在任何平台运行）
- 新建 tests/test_post_process.cpp -- YOLO 后处理单元测试
  - 构造已知的合成 float 张量数据
  - 验证 YOLOv5/v8 解码、NMS、坐标映射
- 新建 tests/test_frame_result_collector.cpp -- 结果聚合测试
  - 单模型/多模型场景
  - 并发 add_result 安全性
- 新建 tests/test_zmq_publisher.cpp -- ZeroMQ 发布/订阅测试
  - 创建 PUB + SUB 对，验证 JSON 消息收发
需要 RKNN 硬件的测试（ARM 设备 + root）
- 新建 tests/test_model_manager.cpp -- 模型加载/dup_context 测试
  - 需要提供 .rknn 模型文件路径作为参数
- 新建 tests/test_infer_pipeline.cpp -- 端到端推理集成测试
  - RTSP -> 解码 -> RGA -> 推理 -> 后处理 -> ZMQ 输出
  - 需要 RTSP 源 + RKNN 模型文件
辅助工具
- 新建 tests/zmq_subscriber.cpp -- 独立 ZMQ 订阅者工具
  - 可独立运行，用于手动验证推理输出
CMakeLists.txt 修改
- 修改 [CMakeLists.txt](CMakeLists.txt):
  - ENABLE_RKNN 改为 ON（默认），添加 find_package(RKNN)
  - ENABLE_ZMQ 改为 ON（默认），添加 find_package(ZMQ)
  - FetchContent 添加 cppzmq（header-only，v4.10.0）
  - 条件添加 inference/ 和 output/ 源文件
  - 链接 RKNN 和 ZMQ 库
- 修改 [tests/CMakeLists.txt](tests/CMakeLists.txt):
  - 添加所有 Phase 3 测试目标
  - 无硬件依赖的测试注册到 CTest
  - 硬件测试仅编译不注册
文档更新
- 修改 [docs/testing_guide.md](docs/testing_guide.md) -- 添加 Phase 3 测试指南
依赖安装（ARM 设备）
# ZeroMQ
sudo apt install libzmq3-dev

# RKNN (已确认存在)
# /usr/local/include/rknn/rknn_api.h
# /usr/lib/librknnrt.so (v2.3.2)
RK3576 NPU 核心绑定策略
Worker 0 -> RKNN_NPU_CORE_0 (mask=1)
Worker 1 -> RKNN_NPU_CORE_1 (mask=2)
Worker 2 -> RKNN_NPU_CORE_2 (mask=4)
若 num_infer_workers < 3，仅使用前 N 个核心；若 > 3，额外 Worker 使用 RKNN_NPU_CORE_AUTO (mask=0) 自动调度。所有 context 通过 rknn_set_core_mask 设定亲和性。
实施顺序
按依赖关系从底层向上实现，每步都有可独立验证的测试。



---

Phase 4: StreamManager + REST API + 系统集成
整体架构
flowchart TB
    Client[HTTP Client] -->|REST API| RestServer
    
    subgraph server [Infer Server Process]
        RestServer[RestServer_cpp-httplib]
        StreamMgr[StreamManager]
        RestServer -->|"add/remove/start/stop"| StreamMgr
        RestServer -->|"get image"| ImgCache[ImageCache]
        
        StreamMgr -->|"owns per-stream"| DT0[DecodeThread_cam01]
        StreamMgr -->|"owns per-stream"| DT1[DecodeThread_cam02]
        StreamMgr -->|"persist"| ConfigMgr[ConfigManager]
        
        subgraph decode [Decode Thread]
            Dec[HwDecoder] --> RGA_infer[RGA_model_size]
            Dec --> RGA_cache[RGA_cache_size]
            RGA_infer --> Submit[InferenceEngine.submit]
            RGA_cache --> Jpeg[JpegEncoder]
            Jpeg --> Cache[ImageCache.add_frame]
        end
        
        InferEngine[InferenceEngine]
        Submit --> InferEngine
        InferEngine -->|ZMQ| Downstream[下游程序]
    end
解码线程流程
每个 RTSP 流对应一个解码线程，内部流程：
flowchart TD
    Start([线程启动]) --> Open[HwDecoder.open]
    Open -->|失败| Reconnect[等待重连]
    Reconnect -->|"backoff: 1/2/4/8s"| Open
    Open -->|成功| Decode[decoder.decode_frame]
    Decode -->|失败| Close[decoder.close]
    Close --> Reconnect
    Decode -->|成功| IncCount["decoded_frames++"]
    IncCount --> SkipCheck{"frame_id % skip == 0?"}
    SkipCheck -->|跳过| Decode
    SkipCheck -->|处理| RgaInfer["RGA: NV12->RGB (model_size)"]
    RgaInfer --> CreateTask[创建InferTask + Collector]
    CreateTask --> SubmitTask[engine.submit]
    SubmitTask --> RgaCache["RGA: NV12->RGB (cache_size)"]
    RgaCache --> JpegEnc[JPEG编码]
    JpegEnc --> AddCache[ImageCache.add_frame]
    AddCache --> Decode
新增/修改文件清单
依赖
- cpp-httplib: FetchContent 获取 (header-only, v0.18.3)
  - 无需额外安装，CMake 自动下载
核心组件 (4 个新文件)
7. StreamManager -- 流生命周期管理
- 新建 [include/infer_server/stream/stream_manager.h](include/infer_server/stream/stream_manager.h)
- 新建 [src/stream/stream_manager.cpp](src/stream/stream_manager.cpp)
class StreamManager {
public:
    StreamManager(const ServerConfig& config,
                  InferenceEngine& engine,
                  ImageCache& cache);
    ~StreamManager();

    // === 流 CRUD ===
    // add_stream: 添加并自动启动流
    bool add_stream(const StreamConfig& config);
    // remove_stream: 停止并移除流
    bool remove_stream(const std::string& cam_id);
    // start/stop 单个流
    bool start_stream(const std::string& cam_id);
    bool stop_stream(const std::string& cam_id);

    // === 批量操作 ===
    void start_all();
    void stop_all();

    // === 查询 ===
    std::vector<StreamStatus> get_all_status() const;
    std::optional<StreamStatus> get_status(const std::string& cam_id) const;
    std::vector<StreamConfig> get_all_configs() const;
    bool has_stream(const std::string& cam_id) const;
    size_t stream_count() const;

    // === 持久化 ===
    void save_configs() const;
    void load_and_start(const std::vector<StreamConfig>& configs);

    void shutdown();
};
内部结构 StreamContext (私有):
struct StreamContext {
    StreamConfig config;
    std::atomic<int> state{(int)StreamState::Stopped};
    std::thread decode_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};

    // 原子统计计数器 (解码线程写, API 线程读)
    std::atomic<uint64_t> decoded_frames{0};
    std::atomic<uint64_t> inferred_frames{0};
    std::atomic<uint32_t> reconnect_count{0};
    std::string last_error;
    std::mutex error_mutex;
    std::chrono::steady_clock::time_point start_time;

    // 每个流拥有独立的 JpegEncoder 实例
    std::unique_ptr<JpegEncoder> jpeg_encoder;
    // 标签缓存: model_path -> labels
    std::unordered_map<std::string, std::vector<std::string>> labels_cache;
};
关键设计:
- streams_ 用 std::unordered_map<std::string, std::unique_ptr<StreamContext>> 存储，受 std::mutex 保护
- 解码线程通过 stop_requested atomic 接收停止信号
- 自动重连: 指数退避 (1s, 2s, 4s, 8s 封顶)
- add_stream 默认自动启动，添加后立即调用 save_configs() 持久化
- 统计信息: decode_fps 通过时间窗口计算 (decoded_frames / uptime)
- inferred_frames 通过 InferenceEngine::set_result_callback() 跟踪 (在回调中按 cam_id 递增)
2. RestServer -- HTTP REST API
- 新建 [include/infer_server/api/rest_server.h](include/infer_server/api/rest_server.h)
- 新建 [src/api/rest_server.cpp](src/api/rest_server.cpp)
REST 端点设计:
暂时无法在飞书文档外展示此内容
请求/响应格式示例:
POST /api/streams:
// Request body = StreamConfig JSON
{
  "cam_id": "cam01",
  "rtsp_url": "rtsp://admin:pass@192.168.254.124:554/...",
  "frame_skip": 5,
  "models": [{
    "model_path": "/weight/yolo.rknn",
    "task_name": "detection",
    "model_type": "yolov5",
    "input_width": 640,
    "input_height": 640
  }]
}
// Response = ApiResponse
{ "code": 0, "message": "Stream cam01 added", "data": {"cam_id": "cam01"} }
GET /api/streams:
{
  "code": 0, "message": "success",
  "data": [/* StreamStatus[] */]
}
GET /api/cache/image?stream_id=cam01&ts=1715600000:
- 成功: Content-Type: image/jpeg, body = JPEG 二进制
- ts 省略或 latest=true: 返回最新帧
- 失败: 404 JSON 错误
GET /api/status:
{
  "code": 0, "message": "success",
  "data": {
    "version": "0.1.0",
    "uptime_seconds": 3600,
    "streams_total": 4,
    "streams_running": 3,
    "infer_queue_size": 2,
    "infer_queue_dropped": 0,
    "infer_total_processed": 15000,
    "zmq_published": 15000,
    "cache_memory_mb": 12.5
  }
}
class RestServer {
public:
    RestServer(StreamManager& mgr, ImageCache& cache,
               InferenceEngine& engine, const ServerConfig& config);
    ~RestServer();
    bool start();  // 在独立线程中启动 httplib::Server::listen
    void stop();
private:
    void setup_routes();
    // ... handlers
};
关键设计:
- cpp-httplib 在独立线程中运行 server->listen()
- 所有 handler 通过 StreamManager 和 ImageCache 的线程安全接口操作
- JSON 序列化使用 nlohmann::json
- 图片接口直接返回 CachedFrame::jpeg_data 的字节流
修改文件
[src/main.cpp](src/main.cpp) -- 完整串联
替换 TODO 占位符，完整生命周期:
1. 加载配置 (ServerConfig)
2. 初始化日志
3. 创建 ImageCache
4. 创建 InferenceEngine, init()
5. 创建 StreamManager(config, engine, cache)
6. 加载持久化流配置 -> StreamManager.load_and_start()
7. 创建 RestServer(stream_mgr, cache, engine, config)
8. RestServer.start()
9. 主循环 (等待信号)
10. RestServer.stop()
11. StreamManager.shutdown()
12. InferenceEngine.shutdown()
13. 日志关闭
[CMakeLists.txt](CMakeLists.txt) -- 添加 cpp-httplib + 新源文件
- FetchContent 添加 cpp-httplib (header-only)
- 新增 option: ENABLE_HTTP (默认 ON)
- 条件添加 src/stream/stream_manager.cpp, src/api/rest_server.cpp
- StreamManager 源文件始终编译 (它条件引用 RKNN/FFmpeg/RGA)
[tests/CMakeLists.txt](tests/CMakeLists.txt) -- 新增测试目标
[docs/testing_guide.md](docs/testing_guide.md) -- Phase 4 测试指南
测试文件
无需全部硬件的测试
- 新建 tests/test_rest_api.cpp -- REST API 单元测试
  - 使用 httplib::Client 在本地测试所有端点
  - 可在开发机上运行 (如果 RKNN/FFmpeg 不可用，流添加会失败但 API 本身可验证)
  - 测试: 添加/删除流、查询状态、错误处理、JSON 格式
需要全部硬件的测试
- 新建 tests/test_system.cpp -- 完整系统集成测试
  - 启动完整服务器 (InferenceEngine + StreamManager + RestServer)
  - 通过 HTTP 添加 RTSP 流
  - 验证解码、推理、ZMQ 输出、图片缓存
  - 测试持久化: 保存配置 -> 恢复 -> 自动启动
  - 测试自动重连: 模拟流中断
依赖安装
# cpp-httplib 通过 CMake FetchContent 自动获取, 无需手动安装
# 其他依赖与 Phase 1-3 相同
标签文件加载策略
在 StreamManager::add_stream() 时为每个模型的 labels_file 加载标签，缓存在 StreamContext::labels_cache 中。解码线程创建 InferTask 时直接引用缓存的标签 vector。
持久化与自动恢复流程
sequenceDiagram
    participant User
    participant REST as RestServer
    participant SM as StreamManager
    participant CM as ConfigManager
    participant Disk as streams.json

    User->>REST: POST /api/streams
    REST->>SM: add_stream(config)
    SM->>SM: create context + start thread
    SM->>CM: save_streams(all_configs)
    CM->>Disk: write JSON

    Note over SM,Disk: 服务器重启...

    SM->>CM: load_streams(path)
    CM->>Disk: read JSON
    CM-->>SM: vector of StreamConfig
    SM->>SM: load_and_start(configs)
    SM->>SM: add_stream each + start threads
实施顺序
1. CMake 更新 (cpp-httplib + 新源文件)
2. StreamManager 实现 (核心编排)
3. RestServer 实现 (HTTP 端点)
4. main.cpp 完整串联
5. test_rest_api.cpp (API 测试)
6. test_system.cpp (集成测试)
7. 文档更新

