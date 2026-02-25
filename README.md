# Infer Server C++

基于 Rockchip 硬件加速的高性能视频推理服务器，支持 RTSP 视频流接入、NPU 推理和实时结果输出。

## 项目简介

Infer Server 是一个专为 Rockchip 平台优化的视频推理服务器，充分利用硬件加速能力提供高效的视频流处理和深度学习推理服务。

### 主要特性

- 🚀 **硬件加速**: 支持 Rockchip FFmpeg 硬件解码、RGA 图像处理和 RKNN NPU 推理
- 📹 **视频流管理**: 支持多路 RTSP 视频流并发处理
- 🔄 **实时推理**: 多模型并行推理，支持目标检测等任务
- 🌐 **REST API**: 提供完整的 HTTP REST API 接口
- 📡 **ZeroMQ 输出**: 支持通过 ZeroMQ 发布推理结果
- 💾 **图像缓存**: JPEG 图像缓存，支持实时预览
- 🔧 **灵活配置**: 可选择性编译各功能模块

## 系统架构

```
RTSP 流 → 硬件解码器 → RGA 处理器 → 推理引擎 → 后处理 → ZeroMQ 发布器
           (FFmpeg-RK)   (librga)     (RKNN NPU)  (NMS等)   (结果输出)
                                           ↓
                                       图像缓存
                                      (TurboJPEG)
                                           ↓
                                      REST API
                                     (HTTP 服务)
```

### 核心组件

| 组件 | 功能 | 依赖 |
|-----|------|------|
| **HwDecoder** | 硬件视频解码 | FFmpeg-RK |
| **RgaProcessor** | 图像格式转换和缩放 | librga |
| **InferenceEngine** | NPU 推理引擎 | librknnrt |
| **PostProcessor** | 推理结果后处理 (NMS等) | - |
| **StreamManager** | 视频流生命周期管理 | - |
| **ImageCache** | 图像缓存和 JPEG 编码 | TurboJPEG |
| **ZmqPublisher** | 结果发布 | ZeroMQ |
| **RestServer** | REST API 服务 | cpp-httplib |

## 构建要求

### 系统依赖

#### 必需依赖
- **CMake** >= 3.16
- **C++ 编译器**: 支持 C++17 (GCC >= 7.0 或 Clang >= 5.0)
- **pthread**: 多线程支持

#### 可选依赖 (硬件加速)
- **FFmpeg-RK**: Rockchip 定制版 FFmpeg (硬件解码)
  - 通常安装在 `/opt/ffmpeg-rk`
  - 或通过 `FFMPEG_RK_ROOT` 环境变量指定路径
- **librga**: Rockchip RGA 图像处理库
  - 安装: `sudo apt install librga-dev`
- **librknnrt**: Rockchip NPU 运行时库
  - 根据板子型号安装对应版本 (RK3588/RK3576等)
- **TurboJPEG**: 高性能 JPEG 编解码
  - 安装: `sudo apt install libturbojpeg0-dev`
- **ZeroMQ**: 消息队列
  - 安装: `sudo apt install libzmq3-dev`

#### 推荐依赖 (使用 RKNN 时)
- **jemalloc**: 替代 glibc malloc，避免 RKNN Runtime 与 glibc 堆元数据冲突导致的崩溃（如 `malloc(): unsorted double linked list corrupted`）
  - 安装: `sudo apt install libjemalloc2`
  - 启动时需使用 `LD_PRELOAD`，见下方「启动服务器」

#### 自动下载依赖 (通过 CMake FetchContent)
- [nlohmann/json](https://github.com/nlohmann/json) v3.11.3
- [spdlog](https://github.com/gabime/spdlog) v1.13.0
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.18.3 (启用 HTTP 时)
- [cppzmq](https://github.com/zeromq/cppzmq) v4.10.0 (启用 ZMQ 时)

## 编译安装

### 1. 克隆项目

```bash
git clone <repository-url>
cd infer-server-cpp
```

### 2. 配置编译选项

项目提供以下 CMake 选项：

| 选项 | 默认值 | 说明 |
|-----|-------|------|
| `BUILD_TESTS` | ON | 编译测试程序 |
| `ENABLE_FFMPEG` | ON | 启用 FFmpeg 硬件解码 |
| `ENABLE_RGA` | ON | 启用 RGA 硬件处理 |
| `ENABLE_RKNN` | ON | 启用 RKNN NPU 推理 |
| `ENABLE_ZMQ` | ON | 启用 ZeroMQ 输出 |
| `ENABLE_HTTP` | ON | 启用 HTTP REST API |

### 3. 编译

#### 完整功能编译 (推荐)

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

#### 仅编译基础功能 (无硬件依赖)

```bash
mkdir build && cd build
cmake .. \
  -DENABLE_FFMPEG=OFF \
  -DENABLE_RGA=OFF \
  -DENABLE_RKNN=OFF \
  -DENABLE_ZMQ=OFF
make -j$(nproc)
```

#### 指定 FFmpeg-RK 路径

```bash
export FFMPEG_RK_ROOT=/path/to/ffmpeg-rk
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 4. 运行测试

```bash
cd build
ctest --output-on-failure
```

### 5. 安装

```bash
sudo make install
```

## 配置文件

配置文件位于 `config/server.json`:

```json
{
  "http_port": 8080,                              // HTTP API 端口
  "zmq_endpoint": "tcp://0.0.0.0:5555",  // ZeroMQ 发布端点 (TCP)
  "num_infer_workers": 3,                         // 推理工作线程数
  "decode_queue_size": 2,                         // 解码队列大小
  "infer_queue_size": 18,                         // 推理队列大小
  "streams_save_path": "/etc/infer-server/streams.json",  // 流配置持久化路径
  "log_level": "info",                            // 日志级别: trace/debug/info/warn/error
  "cache_duration_sec": 5,                        // 图像缓存时长 (秒)
  "cache_jpeg_quality": 75,                       // JPEG 压缩质量 (1-100)
  "cache_resize_width": 640,                      // 缓存图像宽度 (0=不缩放)
  "cache_resize_height": 0,                       // 缓存图像高度 (0=保持比例)
  "cache_max_memory_mb": 64                       // 缓存最大内存 (MB)
}
```

## 使用方法

### 启动服务器

**推荐**：启用 RKNN 推理时，使用 jemalloc 可避免 RKNN Runtime 与 glibc malloc 的兼容性问题导致的崩溃。先安装 `libjemalloc2`，再通过 `LD_PRELOAD` 启动：

```bash
# 使用 jemalloc 启动（推荐，启用 RKNN 时）
sudo apt install -y libjemalloc2
sudo LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2 ./infer_server ../config/server.json
```

其他启动方式：

```bash
# 使用默认配置
./infer_server

# 指定配置文件
./infer_server /path/to/config.json

# 查看帮助
./infer_server --help
```

### REST API 接口

服务器启动后，默认在 `http://localhost:8080` 提供 REST API。

#### 1. 添加视频流

```bash
curl -X POST http://localhost:8080/api/streams \
  -H "Content-Type: application/json" \
  -d '{
    "cam_id": "camera_001",
    "rtsp_url": "rtsp://192.168.1.100:554/stream",
    "frame_skip": 2,
    "models": ["yolov5_person.rknn"]
  }'
```

#### 2. 查询所有视频流

```bash
curl http://localhost:8080/api/streams
```

#### 3. 获取视频流状态

```bash
curl http://localhost:8080/api/streams/camera_001
```

#### 4. 停止视频流

```bash
curl -X DELETE http://localhost:8080/api/streams/camera_001
```

#### 5. 获取实时预览图像

```bash
curl http://localhost:8080/api/streams/camera_001/preview -o preview.jpg
```

#### 6. 查询推理引擎状态

```bash
curl http://localhost:8080/api/inference/status
```

### ZeroMQ 订阅结果

```python
import zmq

context = zmq.Context()
socket = context.socket(zmq.SUB)
socket.connect("tcp://127.0.0.1:5555")
socket.setsockopt_string(zmq.SUBSCRIBE, "")  # 订阅所有消息

while True:
    message = socket.recv_json()
    print(f"Frame {message['frame_id']} from {message['cam_id']}")
    for detection in message['detections']:
        print(f"  - {detection['class_name']}: {detection['confidence']:.2f}")
```

示例订阅程序见 `tests/zmq_subscriber.cpp`。

## 开发文档

### 项目结构

```
infer-server-cpp/
├── cmake/                  # CMake 查找模块
├── config/                 # 配置文件
├── docs/                   # 文档
│   └── testing_guide.md    # 测试指南
├── include/                # 头文件
│   └── infer_server/
│       ├── api/            # REST API
│       ├── cache/          # 图像缓存
│       ├── common/         # 通用组件 (配置、日志、类型)
│       ├── decoder/        # 硬件解码器
│       ├── inference/      # 推理引擎
│       ├── output/         # 输出模块 (ZMQ)
│       ├── processor/      # 图像处理器 (RGA)
│       └── stream/         # 流管理
├── src/                    # 源文件 (与 include 结构对应)
├── tests/                  # 单元测试和集成测试
├── CMakeLists.txt          # 主构建文件
└── README.md               # 本文件
```

### 测试

详细的测试指南请参考 [docs/testing_guide.md](docs/testing_guide.md)。

#### 运行所有测试

```bash
cd build
ctest --output-on-failure
```

#### 运行特定测试

```bash
cd build
./tests/test_bounded_queue      # 队列测试
./tests/test_config             # 配置加载测试
./tests/test_hw_decoder         # 硬件解码器测试
./tests/test_infer_pipeline     # 推理流水线测试
./tests/test_rest_api          # REST API 测试
```

### 日志

项目使用 spdlog 提供结构化日志，支持以下级别：

- `trace`: 详细跟踪信息
- `debug`: 调试信息
- `info`: 一般信息 (默认)
- `warn`: 警告信息
- `error`: 错误信息

日志级别可通过配置文件 `log_level` 字段设置。

## 性能优化建议

### 硬件配置
- **推理工作线程数**: 根据 NPU 核心数设置 (RK3588: 3 核, 建议 2-4 线程)
- **队列大小**: `infer_queue_size` 建议为 `num_infer_workers × 6`
- **帧跳过**: `frame_skip` 设置为 1-3，减少重复帧推理

### 内存优化
- 控制 `cache_max_memory_mb` 避免内存溢出
- 适当减小 `cache_duration_sec` 降低内存占用
- 缩小 `cache_resize_width` 减少缓存图像大小

### 性能监控
- 查看推理队列长度: `/api/inference/status`
- 监控流状态: `/api/streams/{cam_id}`
- 关注日志中的 WARN/ERROR 信息

## 常见问题

### 1. 找不到 FFmpeg-RK 库

```
CMake Warning: FFmpeg-RK not found, hardware decoding disabled.
```

**解决方法**:
- 确认已安装 FFmpeg-RK: `ls /opt/ffmpeg-rk`
- 或设置环境变量: `export FFMPEG_RK_ROOT=/your/path`
- 或禁用 FFmpeg: `cmake -DENABLE_FFMPEG=OFF ..`

### 2. 推理性能不足

**可能原因**:
- NPU 工作线程数不足
- 推理队列堆积
- 模型文件未优化

**解决方法**:
- 增加 `num_infer_workers`
- 增大 `frame_skip` 减少推理频率
- 使用量化后的 RKNN 模型

### 3. 内存占用过高

**解决方法**:
- 减小 `cache_max_memory_mb`
- 减小 `cache_duration_sec`
- 减小 `cache_resize_width`
- 降低 `decode_queue_size` 和 `infer_queue_size`

### 4. 运行时报错 `malloc(): unsorted double linked list corrupted` 并崩溃

这是 RKNN Runtime（librknnrt）与 glibc malloc 堆元数据的兼容性问题，与业务代码无关。

**解决方法**：使用 jemalloc 替代 glibc malloc 启动：

```bash
sudo apt install -y libjemalloc2
sudo LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2 ./infer_server ../config/server.json
```

更多背景与排查过程见项目内的 [CLAUDE.md](CLAUDE.md)。

## 许可证

本项目基于 [MIT License](LICENSE) 开源。

## 贡献指南

欢迎提交 Issue 和 Pull Request！

## 联系方式

- 项目地址: `<repository-url>`
- 问题反馈: `<issues-url>`

## 更新日志

### v0.1.0 (当前版本)
- ✅ 基础架构搭建
- ✅ 硬件解码 (FFmpeg-RK)
- ✅ RGA 图像处理
- ✅ RKNN NPU 推理
- ✅ 多模型并行推理
- ✅ REST API 接口
- ✅ ZeroMQ 结果发布
- ✅ 图像缓存
- ✅ 流配置持久化
- ✅ 完整的测试套件
- ✅ 推荐使用 jemalloc（`LD_PRELOAD`）运行，避免 RKNN 与 glibc malloc 兼容性导致的堆损坏崩溃，详见 [CLAUDE.md](CLAUDE.md)

---

**注意**: 本项目专为 Rockchip 平台优化，在其他平台上可能需要禁用硬件加速相关功能。
