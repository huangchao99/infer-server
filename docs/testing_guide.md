# 推理服务器测试指南

本文档提供详细的分步测试说明，帮助你验证各组件是否正常工作。

## 环境要求

- Ubuntu 22.04 (ARM64, RK3576/RK3588)
- GCC 11+
- CMake 3.16+
- 网络连接 (首次构建需要下载依赖)
- NPU: 通过 /dev/mpp_service 访问 (需要 root 权限)

### Phase 2 额外依赖

```bash
# libjpeg-turbo (JPEG 编码)
# sudo apt install libjpeg-turbo8-dev
sudo apt install libturbojpeg0-dev

# FFmpeg-RK 已安装在 /opt/ffmpeg-rk (需要自行编译安装)
# librga 已安装在系统路径 (随 RK3588 SDK 提供)
```

## 构建项目

```bash
# 1. 进入项目目录
cd /path/to/infer-server

# 2. 创建构建目录
mkdir -p build && cd build

# 3. 配置 (Debug 模式, 方便调试)
# 完整构建 (ARM 设备, 有 FFmpeg/RGA/TurboJPEG):
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 如果 FFmpeg 不在默认路径:
cmake .. -DCMAKE_BUILD_TYPE=Debug -DFFMPEG_RK_ROOT=/opt/ffmpeg-rk

# 最小构建 (无硬件依赖, 仅 Phase 1 + ImageCache):
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_FFMPEG=OFF -DENABLE_RGA=OFF

# 4. 编译
make -j$(nproc)
```

CMake 配置输出会显示各组件是否找到:
```
=== Infer Server Build Configuration ===
  FFmpeg-RK:    ON
  RGA:          ON
  TurboJPEG:    TRUE
```

如果某个组件显示 OFF/FALSE, 请检查安装路径。

---

## 阶段 1: 基础设施测试

### 测试 1.1: 有界队列 (BoundedQueue)

```bash
./tests/test_bounded_queue
```

**预期输出:**
```
========================================
  BoundedQueue Unit Tests
========================================
[RUN ] basic_push_pop
[PASS] basic_push_pop (0ms)
...
========================================
  Results: 12 passed, 0 failed (total 12)
========================================
```

### 测试 1.2: 配置管理

```bash
./tests/test_config
```

**预期输出:**
```
========================================
  Config & Types Unit Tests
========================================
[RUN ] server_config_defaults
[PASS] server_config_defaults (0ms)
...
========================================
  Results: 10 passed, 0 failed (total 10)
========================================
```

### 测试 1.3: 主程序启动

```bash
./infer_server
# 或指定配置文件:
./infer_server ../config/server.json
# Ctrl+C 停止
```

---

## 阶段 2: 解码 + 缩放 + 缓存测试

> **重要**: 以下硬件相关测试需要在 ARM 设备上用 **sudo** 运行, 因为 RKMPP 需要访问 `/dev/mpp` 等设备节点。

### 测试 2.1: 图片缓存 (不需要硬件)

```bash
./tests/test_image_cache
```

**预期输出:**
```
========================================
  ImageCache Unit Tests
========================================
[RUN ] basic_add_get
[PASS] basic_add_get (0ms)
[RUN ] exact_timestamp_lookup
[PASS] exact_timestamp_lookup (0ms)
...
[RUN ] concurrent_multi_stream
    Streams: 4
    Total frames: 400
    Total memory: 200KB
[PASS] concurrent_multi_stream (Xms)
...
========================================
  Results: 9 passed, 0 failed (total 9)
========================================
```

**重点关注:**
- `expire_old_frames`: 验证超过 duration 的帧被自动清理
- `memory_limit_eviction`: 验证内存超限时淘汰最旧帧
- `concurrent_multi_stream`: 验证多线程安全
- `memory_accounting`: 验证内存统计准确

### 测试 2.2: 硬件解码器

```bash
# 替换为你的 RTSP 地址
sudo ./tests/test_hw_decoder "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102" 50
```

**预期输出:**
```
========================================
  HwDecoder Test
========================================
RTSP URL: rtsp://admin:...
Target frames: 50

[TEST] Opening decoder...
[PASS] Decoder opened: 640x360 @ 25.0 fps, codec=h264_rkmpp, hw=yes

[TEST] Decoding 50 frames...
  Frame 0: 640x360, pts=..., size=345600 [size OK] [data OK]
  Frame 1: 640x360, pts=..., size=345600 [size OK] [data OK]
  ...

========================================
  Results
========================================
  Decoded:     50 / 50
  Errors:      0
  Time:        2.0s
  FPS:         25.0
  Total data:  16.5 MB
[PASS] HwDecoder test passed
```

**如果失败:**
- `Failed to open RTSP`: 检查 RTSP 地址是否可达, 网络是否连通
- `h264_rkmpp not found`: FFmpeg 未正确编译 RKMPP 支持, 检查 `/opt/ffmpeg-rk/lib`
- `Permission denied` 或 `/dev/mpp` 错误: 需要 sudo 运行
- `Unexpected pixel format`: RKMPP 解码器输出非 NV12, 请反馈该信息

### 测试 2.3: RGA 处理器

```bash
sudo ./tests/test_rga_processor "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102" ./rga_output
```

**预期输出:**
```
========================================
  RGA Processor Test
========================================
[PASS] Decoded frame: 640x360, NV12 size=345600 bytes

[TEST] RGA NV12(640x360) -> RGB(640x640) ...
       Saved: ./rga_output/rga_640x640.ppm
[PASS] 640x640 (model input) - 0.8ms, 1228800 bytes

[TEST] RGA NV12(640x360) -> RGB(320x320) ...
       Saved: ./rga_output/rga_320x320.ppm
[PASS] 320x320 (small model) - 0.5ms, 307200 bytes

[TEST] RGA NV12(640x360) -> RGB(640x360) ...
       Saved: ./rga_output/rga_640x360.ppm
[PASS] 640x360 (proportional) - 0.6ms, 691200 bytes

========================================
  Results: 3 passed, 0 failed
========================================
Check PPM files in ./rga_output/ for visual verification.
```

**验证 PPM 文件:**
```bash
# 用图片查看器打开 (如 eog, feh, display)
eog ./rga_output/rga_640x640.ppm
# 或者传到有图形界面的电脑查看
```

**如果失败:**
- `RGA imcheck failed`: RGA 参数错误, 检查宽高是否为偶数
- `im2d API not available`: 需要安装支持 im2d 的 librga 版本
- 图片全黑/花屏: 色彩空间转换可能有问题, 需要检查 RGA 格式参数

### 测试 2.4: 完整解码流水线

```bash
sudo ./tests/test_decode_pipeline "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102" 10 ./pipeline_output
```

**预期输出:**
```
========================================
  Decode Pipeline Integration Test
========================================
[OK] Decoder: 640x360 @ 25.0fps
[OK] Cache size: 640x360
[OK] JPEG encoder ready
[OK] Image cache ready (5s buffer, 64MB max)

[RUN] Processing for 10 seconds...
      frame_skip=5, model_input=640x640, cache_size=640x360
  Saved sample JPEG: ./pipeline_output/pipeline_sample.jpg (25000 bytes)
  Processed 20 frames (decoded 100) cache=25 frames mem=625KB
  Processed 40 frames (decoded 200) cache=25 frames mem=625KB

========================================
  Pipeline Statistics
========================================
  Duration:          10.0s
  Total decoded:     250
  Total processed:   50 (skip=5)
  Total skipped:     200
  Decode FPS:        25.0
  Process FPS:       5.0

  Avg RGA infer:     0.8ms
  Avg RGA cache:     0.6ms
  Avg JPEG encode:   2.5ms
  Avg JPEG size:     25000 bytes

  Cache frames:      25
  Cache memory:      625KB

[PASS] Cache latest frame: id=50 ts=... jpeg=25000 bytes
[PASS] Decode pipeline test passed
```

**验证 JPEG 样本:**
```bash
eog ./pipeline_output/pipeline_sample.jpg
# 或传到其他电脑查看
```

**性能参考 (RK3588, 640x360 输入):**
- 解码 FPS: ~25fps (匹配流帧率)
- RGA 缩放: < 2ms
- JPEG 编码: < 5ms (640x360)
- 缓存内存: ~500KB-1MB (5秒, 5fps)

### 运行所有自动测试

```bash
cd build
ctest --verbose
```

这只运行不需要 RTSP 参数的测试 (test_bounded_queue, test_config, test_image_cache)。
硬件测试需要手动指定 RTSP 地址运行。

---

## 常见问题排查

### 编译错误: FFmpeg headers not found

```bash
# 确认 FFmpeg-RK 安装路径
ls /opt/ffmpeg-rk/include/libavformat/
# 如果在不同路径:
cmake .. -DFFMPEG_RK_ROOT=/your/ffmpeg/path
```

### 编译错误: RGA im2d.h not found

```bash
# 检查 RGA 头文件
ls /usr/include/rga/
# 如果有 im2d.h 或 im2d.hpp 就没问题
# 如果只有 rga.h 和 drmrga.h, 需要更新 librga:
# 从 https://github.com/airockchip/librga 编译安装最新版
```

### 编译错误: turbojpeg.h not found

```bash
sudo apt install libjpeg-turbo8-dev
```

### 运行时: Permission denied

```bash
# RKMPP 和 RGA 需要访问设备节点
sudo ./tests/test_hw_decoder ...
# 或者添加用户到 video 组:
sudo usermod -aG video $USER
# 重新登录后生效
```

### 运行时: FFmpeg 库找不到

```bash
# 设置运行时库路径
export LD_LIBRARY_PATH=/opt/ffmpeg-rk/lib:$LD_LIBRARY_PATH
sudo LD_LIBRARY_PATH=/opt/ffmpeg-rk/lib ./tests/test_hw_decoder ...
```

---

## 阶段 3: RKNN 推理 + YOLO 后处理 + ZeroMQ 输出

### Phase 3 额外依赖

```bash
# ZeroMQ (消息队列)
sudo apt install libzmq3-dev

# RKNN Runtime (确认已安装)
ls /usr/local/include/rknn/rknn_api.h    # 头文件
ls /usr/lib/librknnrt.so                  # 运行时库
strings /usr/lib/librknnrt.so | grep "librknnrt version"  # 版本确认
```

### 构建 Phase 3

```bash
cd build

# 完整构建 (ARM 设备, 全部依赖):
cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_RKNN=ON \
    -DENABLE_ZMQ=ON

# 如果 RKNN 头文件不在默认路径:
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRKNN_ROOT=/usr/local

# 最小构建 (仅 Phase 3 无硬件依赖部分):
cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_FFMPEG=OFF -DENABLE_RGA=OFF \
    -DENABLE_RKNN=OFF -DENABLE_ZMQ=ON

make -j$(nproc)
```

CMake 配置输出会显示:
```
=== Infer Server Build Configuration ===
  FFmpeg-RK:    ON
  RGA:          ON
  TurboJPEG:    TRUE
  RKNN:         ON
  ZeroMQ:       ON
```

### 测试 3.1: YOLO 后处理 (不需要硬件)

```bash
./tests/test_post_process
```

**预期输出:**
```
======================================
  Post-Processor Unit Tests
======================================

[TEST] NMS basic - remove overlapping boxes
  PASS

[TEST] NMS - different classes don't suppress each other
  PASS

[TEST] NMS - empty input
  PASS

[TEST] NMS - many overlapping boxes
  After NMS: 1 boxes remain
  PASS

[TEST] INT8 dequantize
  PASS

[TEST] YOLOv5 - synthetic detection
  Detected 1 object(s)
  Detection: class=person conf=0.855 box=(...)
  PASS

[TEST] YOLOv5 - filter low confidence detections
  Detected 0 (expected 0)
  PASS

[TEST] YOLOv8 - synthetic detection
  Detected 1 object(s)
  Detection: class=dog conf=0.92 box=(284,284,364,364)
  PASS

[TEST] Scale coords - letterbox mapping
  PASS

[TEST] process() dispatch - unknown model type returns empty
  PASS

======================================
  Results: 10 passed, 0 failed
======================================
```

**此测试验证:**
- NMS 算法 (同类抑制、跨类不抑制、空输入)
- INT8 反量化公式
- YOLOv5 anchor-based 解码 (sigmoid, grid, anchor)
- YOLOv8 anchor-free DFL 解码
- letterbox 坐标映射
- 模型类型分发

### 测试 3.2: 结果聚合器 (不需要硬件)

```bash
./tests/test_frame_result_collector
```

**预期输出:**
```
======================================
  FrameResultCollector Unit Tests
======================================

[TEST] Single model - immediate completion
  PASS

[TEST] Multi model sequential - completion on last
  PASS

[TEST] Concurrent add_result - thread safety
  PASS

[TEST] Result integrity - all model results preserved
  PASS

[TEST] shared_ptr usage - simulating InferTask aggregator
  PASS

======================================
  Results: 5 passed, 0 failed
======================================
```

**此测试验证:**
- 单模型立即完成
- 多模型最后一个完成时返回
- 多线程并发安全 (恰好一个线程收到完整结果)
- 基础帧信息和所有模型结果完整保留
- shared_ptr<void> 类型转换 (模拟 InferTask::aggregator)

### 测试 3.3: ZMQ 发布器 (需要 libzmq, 不需要 RKNN)

```bash
./tests/test_zmq_publisher
```

**预期输出:**
```
======================================
  ZmqPublisher Unit Tests
======================================

[TEST] Init and shutdown
  PASS

[TEST] PUB/SUB message send and receive
  Received 245 bytes
  JSON parsed and validated OK
  PASS

[TEST] Multiple messages
  Sent 10, received 10
  PASS

[TEST] Concurrent publish (thread safety)
  Threads=4 msgs/thread=10 total_sent=40 received=40
  PASS

[TEST] IPC endpoint (ipc://)
  PASS

======================================
  Results: 5 passed, 0 failed
======================================
```

**此测试验证:**
- ZMQ context/socket 初始化和关闭
- PUB/SUB 消息收发 + JSON 格式验证
- 多消息连续发送
- 多线程并发 publish 线程安全
- IPC endpoint 绑定

### 测试 3.4: 模型管理器 (需要 RKNN 硬件 + .rknn 模型)

```bash
sudo ./tests/test_model_manager /path/to/yolov5.rknn
```

**预期输出:**
```
======================================
  ModelManager Unit Tests
  Model: /path/to/yolov5.rknn
======================================

[TEST] Load RKNN model
  PASS

[TEST] Query model info
  Model: /path/to/yolov5.rknn
  Inputs: 1, Outputs: 3
  Input[0]: dims=[1,640,640,3] n_elems=1228800
  Output[0]: dims=[1,80,80,255] n_elems=1632000 type=3 zp=... scale=...
  Output[1]: dims=[1,40,40,255] n_elems=408000 ...
  Output[2]: dims=[1,20,20,255] n_elems=102000 ...
  TensorAttr conversion OK
  PASS

[TEST] Create worker contexts (dup_context + core binding)
  Worker 0: ctx=... core_mask=1
  Worker 1: ctx=... core_mask=2
  Worker 2: ctx=... core_mask=4
  Worker AUTO: ctx=...
  PASS

[TEST] Worker context inference (dummy input)
  Inference time: 15.2 ms
  Output[0] first 5 values: ...
  PASS

[TEST] Unload model
  PASS

[TEST] Load nonexistent model
  PASS

======================================
  Results: 6 passed, 0 failed
======================================
```

**如果失败:**
- `rknn_init failed`: 模型文件损坏或不兼容当前 NPU 版本
- `Permission denied`: 需要 sudo (访问 /dev/mpp_service)
- `rknn_dup_context failed`: librknnrt 版本过低, 需要 >= 1.4.0
- `rknn_set_core_mask failed`: NPU 核心数不够 (非致命, 自动回退到 AUTO)

### 测试 3.5: 端到端推理流水线 (需要全部硬件)

```bash
sudo ./tests/test_infer_pipeline \
    "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102" \
    /path/to/yolov5.rknn \
    yolov5 \
    30
```

**参数说明:**
- 参数 1: RTSP 地址
- 参数 2: RKNN 模型文件路径
- 参数 3: 模型类型 (yolov5/yolov8/yolov11), 默认 yolov5
- 参数 4: 推理帧数, 默认 30

**预期输出:**
```
======================================
  Phase 3 Integration Pipeline Test
======================================
  RTSP:       rtsp://admin:...
  Model:      /path/to/yolov5.rknn
  Type:       yolov5
  Frames:     30
======================================

[Step 1] Initializing InferenceEngine...

[Step 2] Loading model...
  Model input size: 640x640

[Step 3] Opening RTSP stream...
  Stream: 640x360 @ 25.0 fps codec=h264_rkmpp hw=yes

[Step 4] Running decode->infer pipeline for 30 frames...
  [Result] cam=test_cam frame=0 models=1 dets=3 -> person(0.92) person(0.87) car(0.75)
  [Result] cam=test_cam frame=5 models=1 dets=2 -> person(0.91) person(0.85)
  ...

  Waiting for inference to complete...

======================================
  Pipeline Test Results
======================================
  Frames decoded:    150
  Frames submitted:  30
  Results received:  30
  Total detections:  75
  Queue dropped:     0
  Total processed:   30
  Elapsed:           6.5 s
  Decode FPS:        23.1
  Infer FPS:         4.6
  ZMQ published:     30
======================================

PASS: Pipeline integration test
```

### 测试 3.6: ZMQ 订阅者工具 (手动验证)

在推理流水线运行时, 可以在另一个终端使用独立订阅者监听推理输出:

```bash
# 终端 1: 运行推理 (或后续阶段的完整服务器)
sudo ./tests/test_infer_pipeline "rtsp://..." /model/yolo.rknn yolov5 100

# 终端 2: 监听 ZMQ 输出
./tests/zmq_subscriber ipc:///tmp/infer_server_test_pipeline.ipc
```

**订阅者输出示例:**
```
======================================
  ZMQ Subscriber Tool
  Endpoint: ipc:///tmp/infer_server_test_pipeline.ipc
  Press Ctrl+C to stop
======================================
Connected. Waiting for messages...

[1] [test_cam] frame=0 ts=1700000000000 results=1 detections=3
  [test_detection] infer=15.2ms
    - person conf=0.920 box=[100.5,200.3,350.2,500.1]
    - person conf=0.870 box=[400.0,150.0,550.0,400.0]
    - car conf=0.750 box=[50.0,300.0,200.0,450.0]

[2] [test_cam] frame=5 ts=1700000000200 results=1 detections=2
  ...
```

### 运行所有自动测试 (Phase 1 + 2 + 3)

```bash
cd build
ctest --verbose
```

自动运行的测试 (无需参数):
- test_bounded_queue (Phase 1)
- test_config (Phase 1)
- test_image_cache (Phase 2)
- test_post_process (Phase 3, 新增)
- test_frame_result_collector (Phase 3, 新增)
- test_zmq_publisher (Phase 3, 新增, 需要 libzmq)

手动运行的测试 (需要参数/硬件):
- test_hw_decoder (Phase 2)
- test_rga_processor (Phase 2)
- test_decode_pipeline (Phase 2)
- test_model_manager (Phase 3, 新增)
- test_infer_pipeline (Phase 3, 新增)

---

## 常见问题排查

### 编译错误: FFmpeg headers not found

```bash
# 确认 FFmpeg-RK 安装路径
ls /opt/ffmpeg-rk/include/libavformat/
# 如果在不同路径:
cmake .. -DFFMPEG_RK_ROOT=/your/ffmpeg/path
```

### 编译错误: RGA im2d.h not found

```bash
# 检查 RGA 头文件
ls /usr/include/rga/
# 如果有 im2d.h 或 im2d.hpp 就没问题
# 如果只有 rga.h 和 drmrga.h, 需要更新 librga:
# 从 https://github.com/airockchip/librga 编译安装最新版
```

### 编译错误: turbojpeg.h not found

```bash
sudo apt install libjpeg-turbo8-dev
```

### 编译错误: rknn_api.h not found

```bash
# 确认 RKNN 头文件路径
ls /usr/local/include/rknn/rknn_api.h
# 如果在其他路径:
cmake .. -DRKNN_ROOT=/your/rknn/path
```

### 编译错误: zmq.h not found

```bash
sudo apt install libzmq3-dev
```

### 运行时: Permission denied

```bash
# RKMPP, RGA 和 RKNN 需要访问设备节点 /dev/mpp_service
sudo ./tests/test_hw_decoder ...
# 或者添加用户到 video 组:
sudo usermod -aG video $USER
# 重新登录后生效
```

### 运行时: FFmpeg 库找不到

```bash
# 设置运行时库路径
export LD_LIBRARY_PATH=/opt/ffmpeg-rk/lib:$LD_LIBRARY_PATH
sudo LD_LIBRARY_PATH=/opt/ffmpeg-rk/lib ./tests/test_hw_decoder ...
```

### 运行时: rknn_init failed

```bash
# 检查模型文件是否完整
ls -la /path/to/model.rknn
# 确认模型是否兼容当前 NPU (RK3576)
# 模型需要用 rknn-toolkit2 针对 rk3576 平台导出
```

### 运行时: ZMQ bind failed

```bash
# IPC 文件权限问题
ls -la /tmp/infer_server.ipc
# 删除残留的 IPC 文件
rm -f /tmp/infer_server.ipc
```

---

## 阶段 4: StreamManager + REST API + 系统集成

阶段 4 新增组件:
- **StreamManager**: 流生命周期管理 (添加/删除/启停、解码线程编排、自动重连、统计)
- **RestServer**: HTTP REST API 服务器 (基于 cpp-httplib)
- **任务持久化**: 流配置自动保存/恢复
- **main.cpp 完整串联**: 所有组件的创建、初始化和优雅关闭

### 新增依赖

```bash
# cpp-httplib 通过 CMake FetchContent 自动获取, 无需手动安装
# 其他依赖与 Phase 1-3 相同
```

### 构建 (含 Phase 4)

```bash
cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_FFMPEG=ON \
  -DENABLE_RGA=ON \
  -DENABLE_RKNN=ON \
  -DENABLE_ZMQ=ON \
  -DENABLE_HTTP=ON \
  -DBUILD_TESTS=ON

make -j$(nproc)
```

### REST API 端点一览

| 方法   | 路径                        | 说明               |
|--------|-----------------------------|--------------------|
| POST   | `/api/streams`              | 添加流 (含自动启动) |
| DELETE | `/api/streams/:cam_id`      | 移除流             |
| GET    | `/api/streams`              | 获取所有流状态     |
| GET    | `/api/streams/:cam_id`      | 获取单个流状态     |
| POST   | `/api/streams/:cam_id/start`| 启动流             |
| POST   | `/api/streams/:cam_id/stop` | 停止流             |
| POST   | `/api/streams/start_all`    | 启动所有流         |
| POST   | `/api/streams/stop_all`     | 停止所有流         |
| GET    | `/api/status`               | 服务器全局状态     |
| GET    | `/api/cache/image`          | 获取缓存图片       |

---

### 4.1 REST API 单元测试 (test_rest_api)

**不需要 RKNN/FFmpeg 等硬件**, 可在任意 Linux 开发机上运行。
测试 API 层面的请求/响应正确性。

```bash
cd build
./tests/test_rest_api
```

**期望输出:**

```
=== REST API Unit Tests ===

[Test 1] GET /api/status
  Response: {"code":0, "data":{"streams_total":0, ...}, "message":"success"}

[Test 2] GET /api/streams (empty)

[Test 3] POST /api/streams (add cam01)
  Response: {"code":0, "data":{"cam_id":"cam01"}, "message":"Stream cam01 added"}

[Test 4] POST /api/streams (duplicate cam01)
  Response: {"code":409, "data":{}, "message":"Stream cam01 already exists"}

...

=== Results ===
PASSED: N
FAILED: 0
ALL TESTS PASSED!
```

### 4.2 完整系统集成测试 (test_system)

**需要全部硬件** (RKNN + FFmpeg + RGA + TurboJPEG + ZMQ) + RTSP 摄像头 + RKNN 模型。

```bash
cd build
sudo ./tests/test_system \
  "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102" \
  "/path/to/model.rknn" \
  "yolov5" \
  "cam01"
```

**期望输出:**

```
=== System Integration Test ===
RTSP:  rtsp://admin:hifleet321@192.168.254.124:554/...
Model: /path/to/model.rknn

[Test 1] Add stream via POST /api/streams
  Response: {"code":0, "data":{"cam_id":"cam01"}, "message":"Stream cam01 added"}

[Test 2] Waiting 8 seconds for decode & inference...

[Test 3] GET /api/streams/cam01
  Status:         running
  Decoded frames: 200
  Inferred frames: 38
  Decode FPS:     25.1
  Infer FPS:      4.8

[Test 4] GET /api/status
  Server status: { "version":"0.1.0", "streams_total":1, ... }

[Test 5] GET /api/cache/image?stream_id=cam01
  Got JPEG image: 34521 bytes
  Saved to /tmp/test_system_cached.jpg

...

=== System Integration Test Results ===
PASSED: N
FAILED: 0
ALL TESTS PASSED!
```

### 4.3 使用 curl 手动测试 REST API

启动服务器:

```bash
cd build
sudo ./infer_server ../config/server.json
```

#### 添加流

```bash
curl -X POST http://localhost:8080/api/streams \
  -H "Content-Type: application/json" \
  -d '{
    "cam_id": "cam01",
    "rtsp_url": "rtsp://admin:hifleet321@192.168.254.124:554/Streaming/Channels/102",
    "frame_skip": 5,
    "models": [{
      "model_path": "/path/to/model.rknn",
      "task_name": "detection",
      "model_type": "yolov5",
      "input_width": 640,
      "input_height": 640,
      "conf_threshold": 0.25,
      "nms_threshold": 0.45
    }]
  }'
```

**响应:**
```json
{"code":0,"data":{"cam_id":"cam01"},"message":"Stream cam01 added"}
```

#### 查询所有流状态

```bash
curl http://localhost:8080/api/streams | python3 -m json.tool
```

**响应:**
```json
{
  "code": 0,
  "message": "success",
  "data": [{
    "cam_id": "cam01",
    "rtsp_url": "rtsp://...",
    "status": "running",
    "decoded_frames": 500,
    "inferred_frames": 100,
    "decode_fps": 25.0,
    "infer_fps": 5.0,
    "reconnect_count": 0,
    "uptime_seconds": 20.0
  }]
}
```

#### 查询单个流状态

```bash
curl http://localhost:8080/api/streams/cam01 | python3 -m json.tool
```

#### 查询服务器全局状态

```bash
curl http://localhost:8080/api/status | python3 -m json.tool
```

**响应:**
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "version": "0.1.0",
    "uptime_seconds": 60.5,
    "streams_total": 1,
    "streams_running": 1,
    "infer_queue_size": 0,
    "infer_queue_dropped": 0,
    "infer_total_processed": 100,
    "zmq_published": 100,
    "cache_memory_mb": 5.2,
    "cache_total_frames": 100
  }
}
```

#### 获取缓存图片

```bash
# 获取最新帧
curl -o /tmp/cached.jpg http://localhost:8080/api/cache/image?stream_id=cam01

# 获取指定时间戳附近的帧
curl -o /tmp/cached.jpg "http://localhost:8080/api/cache/image?stream_id=cam01&ts=1715600000000"

# 使用 file 命令验证 JPEG
file /tmp/cached.jpg
# 输出: /tmp/cached.jpg: JPEG image data, ...
```

#### 停止/启动流

```bash
# 停止
curl -X POST http://localhost:8080/api/streams/cam01/stop

# 启动
curl -X POST http://localhost:8080/api/streams/cam01/start

# 停止所有
curl -X POST http://localhost:8080/api/streams/stop_all

# 启动所有
curl -X POST http://localhost:8080/api/streams/start_all
```

#### 删除流

```bash
curl -X DELETE http://localhost:8080/api/streams/cam01
```

#### 添加多个流

```bash
# 添加第二个流
curl -X POST http://localhost:8080/api/streams \
  -H "Content-Type: application/json" \
  -d '{
    "cam_id": "cam02",
    "rtsp_url": "rtsp://admin:hifleet321@192.168.254.123:554/Streaming/Channels/101",
    "frame_skip": 5,
    "models": [{
      "model_path": "/path/to/model.rknn",
      "task_name": "detection",
      "model_type": "yolov5",
      "input_width": 640,
      "input_height": 640
    }]
  }'
```

### 4.4 持久化与自动恢复测试

1. 启动服务器并添加流:

```bash
sudo ./infer_server ../config/server.json
# 在另一个终端:
curl -X POST http://localhost:8080/api/streams -H "Content-Type: application/json" \
  -d '{"cam_id":"cam01","rtsp_url":"rtsp://...","frame_skip":5,"models":[...]}'
```

2. 验证持久化文件已创建:

```bash
cat /etc/infer-server/streams.json | python3 -m json.tool
```

3. Ctrl+C 停止服务器, 然后重启:

```bash
sudo ./infer_server ../config/server.json
```

4. 观察日志, 应该看到自动恢复:

```
[info] Found 1 persisted stream(s), auto-starting...
[info]   - [cam01] rtsp://... (1 model(s), skip=5)
[info] Loading 1 persisted stream(s)...
[info] Adding stream: [cam01] rtsp://... (skip=5, 1 model(s))
[info] [cam01] Stream opened: 640x360 @ 25.0fps codec=h264_rkmpp hw=yes
```

5. 验证流已自动启动:

```bash
curl http://localhost:8080/api/streams | python3 -m json.tool
```

### 4.5 ZMQ 输出验证 (配合 zmq_subscriber)

在另一个终端运行 ZMQ 订阅者:

```bash
sudo ./tests/zmq_subscriber
```

然后在服务器中添加流, 应该能看到推理结果 JSON 输出。

---

### Phase 4 故障排除

#### REST API 无法访问

```bash
# 检查端口是否被占用
ss -tlnp | grep 8080

# 检查防火墙
sudo ufw status

# 修改配置文件中的端口
# config/server.json -> "http_port": 8081
```

#### 流添加后状态一直是 reconnecting

```bash
# 检查 RTSP 地址是否可达
ffmpeg -rtsp_transport tcp -i "rtsp://..." -c:v rawvideo -f null - 2>&1 | head -20

# 检查解码器是否可用
sudo ls -la /dev/mpp_service
```

#### 持久化文件创建失败

```bash
# 确保目录存在且有写权限
sudo mkdir -p /etc/infer-server
sudo chmod 755 /etc/infer-server

# 或修改配置使用本地路径
# "streams_save_path": "./streams.json"
```

#### 缓存图片返回 503

```
说明 TurboJPEG 或 RGA 未编译/不可用。
检查构建输出中 TurboJPEG 和 RGA 是否被检测到。
```

---

## 完整构建摘要

```bash
# 安装所有依赖 (RK3576/RK3588 设备上)
sudo apt install -y libzmq3-dev libturbojpeg0-dev

# 完整构建
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_FFMPEG=ON \
  -DENABLE_RGA=ON \
  -DENABLE_RKNN=ON \
  -DENABLE_ZMQ=ON \
  -DENABLE_HTTP=ON \
  -DBUILD_TESTS=ON

make -j$(nproc)

# 运行所有不需要硬件的测试
ctest --output-on-failure

# 测试输出应该包括:
# test_bounded_queue ... Passed
# test_config        ... Passed
# test_image_cache   ... Passed
# test_post_process  ... Passed
# test_frame_result_collector ... Passed
# test_zmq_publisher ... Passed
# test_rest_api      ... Passed

# 运行需要硬件的集成测试
sudo ./tests/test_system "rtsp://..." "/path/to/model.rknn" "yolov5"

# 启动正式服务
sudo ./infer_server ../config/server.json
```
