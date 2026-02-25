# Infer Server REST API 接口文档

**版本**: v0.1.0  
**基础路径**: `http://{host}:{port}/api`  
**默认端口**: 8080

---

## 目录

- [1. 概述](#1-概述)
- [2. 通用说明](#2-通用说明)
  - [2.1 请求格式](#21-请求格式)
  - [2.2 响应格式](#22-响应格式)
  - [2.3 状态码](#23-状态码)
  - [2.4 错误处理](#24-错误处理)
- [3. 流管理接口](#3-流管理接口)
  - [3.1 添加视频流](#31-添加视频流)
  - [3.2 删除视频流](#32-删除视频流)
  - [3.3 获取所有流状态](#33-获取所有流状态)
  - [3.4 获取单个流状态](#34-获取单个流状态)
  - [3.5 启动单个流](#35-启动单个流)
  - [3.6 停止单个流](#36-停止单个流)
  - [3.7 启动所有流](#37-启动所有流)
  - [3.8 停止所有流](#38-停止所有流)
- [4. 状态查询接口](#4-状态查询接口)
  - [4.1 获取服务器全局状态](#41-获取服务器全局状态)
- [5. 图像缓存接口](#5-图像缓存接口)
  - [5.1 获取缓存图像](#51-获取缓存图像)
- [6. 数据模型](#6-数据模型)
  - [6.1 StreamConfig](#61-streamconfig)
  - [6.2 ModelConfig](#62-modelconfig)
  - [6.3 StreamStatus](#63-streamstatus)
  - [6.4 ApiResponse](#64-apiresponse)
- [7. ZeroMQ 结果订阅](#7-zeromq-结果订阅)
  - [7.1 FrameResult](#71-frameresult)
  - [7.2 订阅示例](#72-订阅示例)
- [8. 完整使用示例](#8-完整使用示例)

---

## 1. 概述

Infer Server 提供完整的 REST API 接口，用于管理 RTSP 视频流的接入、推理任务配置和状态监控。

### 主要功能

- **流管理**: 添加、删除、启动、停止 RTSP 视频流
- **状态监控**: 查询流状态、服务器状态、推理队列状态
- **图像获取**: 获取实时缓存的 JPEG 图像
- **配置持久化**: 流配置自动保存，重启后自动恢复

### 技术栈

- **HTTP 服务器**: cpp-httplib (header-only)
- **数据格式**: JSON (nlohmann/json)
- **图像格式**: JPEG (TurboJPEG)
- **结果发布**: ZeroMQ (可选)

---

## 2. 通用说明

### 2.1 请求格式

#### 请求头

```http
Content-Type: application/json
```

#### 请求体

所有 POST 请求均使用 JSON 格式：

```json
{
  "cam_id": "camera_001",
  "rtsp_url": "rtsp://192.168.1.100:554/stream",
  "frame_skip": 2,
  "models": [...]
}
```

### 2.2 响应格式

#### 统一响应结构

所有接口响应均遵循以下格式：

```json
{
  "code": 0,
  "message": "success",
  "data": { ... }
}
```

**字段说明**:
- `code` (int): 状态码，0 表示成功，非 0 表示失败
- `message` (string): 描述信息
- `data` (object): 返回的数据内容

#### 响应头

```http
Content-Type: application/json
```

### 2.3 状态码

| HTTP 状态码 | 说明 |
|-----------|------|
| 200 | 成功 |
| 400 | 请求参数错误 |
| 404 | 资源不存在 |
| 409 | 资源冲突（如流已存在）|
| 500 | 服务器内部错误 |
| 503 | 服务不可用（如功能未编译）|

### 2.4 错误处理

#### 错误响应示例

```json
{
  "code": 400,
  "message": "cam_id is required",
  "data": {}
}
```

#### 常见错误

| code | message | 说明 |
|------|---------|------|
| 400 | Invalid JSON | JSON 格式错误 |
| 400 | cam_id is required | 缺少必需参数 |
| 404 | Stream not found | 流不存在 |
| 409 | Stream already exists | 流已存在 |
| 500 | Failed to add stream | 流添加失败 |
| 503 | Image cache not available | 图像缓存功能未启用 |

---

## 3. 流管理接口

### 3.1 添加视频流

添加一路 RTSP 视频流，并自动启动解码和推理。

#### 请求

```http
POST /api/streams
Content-Type: application/json
```

**请求体**:

```json
{
  "cam_id": "camera_001",
  "rtsp_url": "rtsp://192.168.1.100:554/stream",
  "frame_skip": 2,
  "models": [
    {
      "model_path": "/path/to/yolov5.rknn",
      "task_name": "person_detection",
      "model_type": "yolov5",
      "input_width": 640,
      "input_height": 640,
      "conf_threshold": 0.25,
      "nms_threshold": 0.45,
      "labels_file": "/path/to/coco.names"
    }
  ]
}
```

**参数说明**:
- `cam_id` (string, 必需): 摄像头唯一标识符
- `rtsp_url` (string, 必需): RTSP 流地址
- `frame_skip` (int, 可选): 每 N 帧推理一次，默认 5
- `models` (array, 可选): 模型配置列表，详见 [ModelConfig](#62-modelconfig)

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "Stream camera_001 added",
  "data": {
    "cam_id": "camera_001"
  }
}
```

**失败示例**:

```json
// 409 - 流已存在
{
  "code": 409,
  "message": "Stream camera_001 already exists",
  "data": {}
}

// 400 - 缺少参数
{
  "code": 400,
  "message": "rtsp_url is required",
  "data": {}
}
```

#### curl 示例

```bash
curl -X POST http://localhost:8080/api/streams \
  -H "Content-Type: application/json" \
  -d '{
    "cam_id": "camera_001",
    "rtsp_url": "rtsp://192.168.1.100:554/stream",
    "frame_skip": 2,
    "models": [
      {
        "model_path": "/models/yolov5.rknn",
        "task_name": "person_detection",
        "model_type": "yolov5",
        "conf_threshold": 0.3
      }
    ]
  }'
```

---

### 3.2 删除视频流

停止并删除指定视频流。

#### 请求

```http
DELETE /api/streams/{cam_id}
```

**路径参数**:
- `cam_id` (string): 摄像头标识符

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "Stream camera_001 removed",
  "data": {
    "cam_id": "camera_001"
  }
}
```

**失败 (404)**:

```json
{
  "code": 404,
  "message": "Stream camera_001 not found",
  "data": {}
}
```

#### curl 示例

```bash
curl -X DELETE http://localhost:8080/api/streams/camera_001
```

---

### 3.3 获取所有流状态

获取所有已注册视频流的状态信息。

#### 请求

```http
GET /api/streams
```

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "success",
  "data": [
    {
      "cam_id": "camera_001",
      "rtsp_url": "rtsp://192.168.1.100:554/stream",
      "status": "running",
      "frame_skip": 2,
      "models": [...],
      "decoded_frames": 1523,
      "inferred_frames": 761,
      "dropped_frames": 0,
      "decode_fps": 25.3,
      "infer_fps": 12.6,
      "reconnect_count": 0,
      "last_error": "",
      "uptime_seconds": 60.5
    },
    {
      "cam_id": "camera_002",
      "rtsp_url": "rtsp://192.168.1.101:554/stream",
      "status": "stopped",
      "frame_skip": 5,
      "models": [...],
      "decoded_frames": 0,
      "inferred_frames": 0,
      "dropped_frames": 0,
      "decode_fps": 0.0,
      "infer_fps": 0.0,
      "reconnect_count": 0,
      "last_error": "",
      "uptime_seconds": 0.0
    }
  ]
}
```

**字段说明**: 详见 [StreamStatus](#63-streamstatus)

#### curl 示例

```bash
curl http://localhost:8080/api/streams
```

---

### 3.4 获取单个流状态

获取指定视频流的详细状态信息。

#### 请求

```http
GET /api/streams/{cam_id}
```

**路径参数**:
- `cam_id` (string): 摄像头标识符

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "cam_id": "camera_001",
    "rtsp_url": "rtsp://192.168.1.100:554/stream",
    "status": "running",
    "frame_skip": 2,
    "models": [
      {
        "model_path": "/models/yolov5.rknn",
        "task_name": "person_detection",
        "model_type": "yolov5",
        "input_width": 640,
        "input_height": 640,
        "conf_threshold": 0.25,
        "nms_threshold": 0.45,
        "labels_file": "/models/coco.names"
      }
    ],
    "decoded_frames": 1523,
    "inferred_frames": 761,
    "dropped_frames": 0,
    "decode_fps": 25.3,
    "infer_fps": 12.6,
    "reconnect_count": 0,
    "last_error": "",
    "uptime_seconds": 60.5
  }
}
```

**失败 (404)**:

```json
{
  "code": 404,
  "message": "Stream camera_999 not found",
  "data": {}
}
```

#### curl 示例

```bash
curl http://localhost:8080/api/streams/camera_001
```

---

### 3.5 启动单个流

启动已停止的视频流。

#### 请求

```http
POST /api/streams/{cam_id}/start
```

**路径参数**:
- `cam_id` (string): 摄像头标识符

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "Stream camera_001 started",
  "data": {
    "cam_id": "camera_001"
  }
}
```

**失败 (404)**:

```json
{
  "code": 404,
  "message": "Stream camera_001 not found or already running",
  "data": {}
}
```

#### curl 示例

```bash
curl -X POST http://localhost:8080/api/streams/camera_001/start
```

---

### 3.6 停止单个流

停止正在运行的视频流（不删除配置）。

#### 请求

```http
POST /api/streams/{cam_id}/stop
```

**路径参数**:
- `cam_id` (string): 摄像头标识符

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "Stream camera_001 stopped",
  "data": {
    "cam_id": "camera_001"
  }
}
```

**失败 (404)**:

```json
{
  "code": 404,
  "message": "Stream camera_001 not found",
  "data": {}
}
```

#### curl 示例

```bash
curl -X POST http://localhost:8080/api/streams/camera_001/stop
```

---

### 3.7 启动所有流

启动所有已注册但未运行的视频流。

#### 请求

```http
POST /api/streams/start_all
```

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "All streams started",
  "data": {}
}
```

#### curl 示例

```bash
curl -X POST http://localhost:8080/api/streams/start_all
```

---

### 3.8 停止所有流

停止所有正在运行的视频流。

#### 请求

```http
POST /api/streams/stop_all
```

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "All streams stopped",
  "data": {}
}
```

#### curl 示例

```bash
curl -X POST http://localhost:8080/api/streams/stop_all
```

---

## 4. 状态查询接口

### 4.1 获取服务器全局状态

获取服务器运行状态、推理队列状态和资源使用情况。

#### 请求

```http
GET /api/status
```

#### 响应

**成功 (200)**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "version": "0.1.0",
    "uptime_seconds": 3625.8,
    "streams_total": 5,
    "streams_running": 3,
    "infer_queue_size": 12,
    "infer_queue_dropped": 0,
    "infer_total_processed": 45231,
    "zmq_published": 45231,
    "cache_memory_mb": 45.67,
    "cache_total_frames": 215
  }
}
```

**字段说明**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `version` | string | 服务器版本 |
| `uptime_seconds` | number | 服务器运行时长（秒）|
| `streams_total` | int | 已注册流总数 |
| `streams_running` | int | 正在运行的流数量 |
| `infer_queue_size` | int | 当前推理队列中的任务数 |
| `infer_queue_dropped` | int | 因队列满而丢弃的任务数 |
| `infer_total_processed` | int | 累计处理的推理任务数 |
| `zmq_published` | int | ZeroMQ 发布的消息数（需启用 ZMQ）|
| `cache_memory_mb` | number | 图像缓存占用内存（MB，需启用缓存）|
| `cache_total_frames` | int | 缓存中的总帧数（需启用缓存）|

**注意**: 
- `infer_*` 字段仅在启用 RKNN 推理时可用
- `zmq_*` 字段仅在启用 ZeroMQ 时可用
- `cache_*` 字段仅在启用 TurboJPEG 缓存时可用

#### curl 示例

```bash
curl http://localhost:8080/api/status
```

---

## 5. 图像缓存接口

### 5.1 获取缓存图像

获取指定视频流的缓存 JPEG 图像（用于预览）。

#### 请求

```http
GET /api/cache/image?stream_id={cam_id}&latest=true
```

**查询参数**:
- `stream_id` (string, 必需): 摄像头标识符
- `latest` (boolean, 可选): 是否获取最新帧，默认 `true`
- `ts` (int64, 可选): 时间戳（毫秒），获取最接近该时间的帧

**参数组合说明**:
1. `stream_id=xxx&latest=true` - 获取最新帧（推荐）
2. `stream_id=xxx` - 等价于 `latest=true`
3. `stream_id=xxx&ts=1234567890` - 获取指定时间戳附近的帧

#### 响应

**成功 (200)**:

```http
HTTP/1.1 200 OK
Content-Type: image/jpeg
X-Frame-Id: 1523
X-Timestamp-Ms: 1707734400000
X-Width: 1920
X-Height: 1080

<JPEG 二进制数据>
```

**响应头说明**:
- `Content-Type`: `image/jpeg`
- `X-Frame-Id`: 帧序号
- `X-Timestamp-Ms`: 时间戳（毫秒）
- `X-Width`: 图像宽度
- `X-Height`: 图像高度

**失败示例**:

```json
// 404 - 无缓存图像
{
  "code": 404,
  "message": "No cached image found for stream camera_001",
  "data": {}
}

// 400 - 缺少参数
{
  "code": 400,
  "message": "stream_id parameter is required",
  "data": {}
}

// 503 - 功能未启用
{
  "code": 503,
  "message": "Image cache not available",
  "data": {}
}
```

#### curl 示例

```bash
# 获取最新帧
curl "http://localhost:8080/api/cache/image?stream_id=camera_001&latest=true" \
  -o preview.jpg

# 获取指定时间戳的帧
curl "http://localhost:8080/api/cache/image?stream_id=camera_001&ts=1707734400000" \
  -o frame.jpg

# 查看响应头
curl -I "http://localhost:8080/api/cache/image?stream_id=camera_001&latest=true"
```

#### JavaScript 示例

```javascript
// 在网页中显示实时预览
async function loadPreview(camId) {
  const url = `http://localhost:8080/api/cache/image?stream_id=${camId}&latest=true`;
  const response = await fetch(url);
  
  if (response.ok) {
    const blob = await response.blob();
    const frameId = response.headers.get('X-Frame-Id');
    const timestamp = response.headers.get('X-Timestamp-Ms');
    
    const img = document.getElementById('preview');
    img.src = URL.createObjectURL(blob);
    
    console.log(`Frame ${frameId} at ${new Date(parseInt(timestamp))}`);
  }
}

// 每秒刷新一次
setInterval(() => loadPreview('camera_001'), 1000);
```

---

## 6. 数据模型

### 6.1 StreamConfig

视频流配置结构。

```json
{
  "cam_id": "camera_001",
  "rtsp_url": "rtsp://192.168.1.100:554/stream",
  "frame_skip": 5,
  "models": [...]
}
```

**字段说明**:

| 字段 | 类型 | 必需 | 默认值 | 说明 |
|-----|------|------|--------|------|
| `cam_id` | string | 是 | - | 摄像头唯一标识符 |
| `rtsp_url` | string | 是 | - | RTSP 流地址 |
| `frame_skip` | int | 否 | 5 | 每 N 帧推理一次（跳帧策略）|
| `models` | array | 否 | [] | 模型配置列表，详见 [ModelConfig](#62-modelconfig) |

---

### 6.2 ModelConfig

推理模型配置结构。

```json
{
  "model_path": "/path/to/yolov5.rknn",
  "task_name": "person_detection",
  "model_type": "yolov5",
  "input_width": 640,
  "input_height": 640,
  "conf_threshold": 0.25,
  "nms_threshold": 0.45,
  "labels_file": "/path/to/coco.names"
}
```

**字段说明**:

| 字段 | 类型 | 必需 | 默认值 | 说明 |
|-----|------|------|--------|------|
| `model_path` | string | 是 | - | RKNN 模型文件路径 |
| `task_name` | string | 是 | - | 任务名称标识（用于区分多模型结果）|
| `model_type` | string | 否 | "yolov5" | 模型类型：`yolov5` / `yolov8` / `yolov11` |
| `input_width` | int | 否 | 640 | 模型输入宽度 |
| `input_height` | int | 否 | 640 | 模型输入高度 |
| `conf_threshold` | float | 否 | 0.25 | 置信度阈值（0.0-1.0）|
| `nms_threshold` | float | 否 | 0.45 | NMS 阈值（0.0-1.0）|
| `labels_file` | string | 否 | "" | 类别标签文件路径（每行一个类别名）|

**模型类型说明**:
- `yolov5`: YOLOv5 系列模型
- `yolov8`: YOLOv8 系列模型
- `yolov11`: YOLOv11 系列模型

---

### 6.3 StreamStatus

视频流运行状态结构。

```json
{
  "cam_id": "camera_001",
  "rtsp_url": "rtsp://192.168.1.100:554/stream",
  "status": "running",
  "frame_skip": 2,
  "models": [...],
  "decoded_frames": 1523,
  "inferred_frames": 761,
  "dropped_frames": 0,
  "decode_fps": 25.3,
  "infer_fps": 12.6,
  "reconnect_count": 0,
  "last_error": "",
  "uptime_seconds": 60.5
}
```

**字段说明**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `cam_id` | string | 摄像头标识符 |
| `rtsp_url` | string | RTSP 流地址 |
| `status` | string | 流状态：`stopped` / `starting` / `running` / `reconnecting` / `error` |
| `frame_skip` | int | 跳帧间隔 |
| `models` | array | 模型配置列表 |
| `decoded_frames` | uint64 | 累计解码帧数 |
| `inferred_frames` | uint64 | 累计推理帧数 |
| `dropped_frames` | uint64 | 累计丢弃帧数（解码队列满）|
| `decode_fps` | number | 解码帧率 |
| `infer_fps` | number | 推理帧率 |
| `reconnect_count` | uint32 | 重连次数 |
| `last_error` | string | 最后一次错误信息 |
| `uptime_seconds` | number | 运行时长（秒）|

**状态说明**:
- `stopped`: 已停止
- `starting`: 启动中
- `running`: 正常运行
- `reconnecting`: 重连中（RTSP 连接断开）
- `error`: 发生错误

---

### 6.4 ApiResponse

统一 API 响应结构。

```json
{
  "code": 0,
  "message": "success",
  "data": { ... }
}
```

**字段说明**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `code` | int | 状态码，0 表示成功，非 0 表示失败 |
| `message` | string | 描述信息 |
| `data` | object | 返回数据（类型取决于具体接口）|

---

## 7. ZeroMQ 结果订阅

除了 REST API，Infer Server 还支持通过 ZeroMQ 发布推理结果，适用于需要实时接收推理结果的场景。

### 7.1 FrameResult

推理结果数据结构（通过 ZeroMQ 发布）。

```json
{
  "cam_id": "camera_001",
  "rtsp_url": "rtsp://192.168.1.100:554/stream",
  "frame_id": 1523,
  "timestamp_ms": 1707734400000,
  "pts": 123456789,
  "original_width": 1920,
  "original_height": 1080,
  "results": [
    {
      "task_name": "person_detection",
      "model_path": "/models/yolov5.rknn",
      "inference_time_ms": 15.6,
      "detections": [
        {
          "class_id": 0,
          "class_name": "person",
          "confidence": 0.89,
          "bbox": {
            "x1": 100.5,
            "y1": 200.3,
            "x2": 350.7,
            "y2": 650.1
          }
        },
        {
          "class_id": 0,
          "class_name": "person",
          "confidence": 0.76,
          "bbox": {
            "x1": 800.2,
            "y1": 150.6,
            "x2": 1020.8,
            "y2": 580.4
          }
        }
      ]
    }
  ]
}
```

**字段说明**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `cam_id` | string | 摄像头标识符 |
| `rtsp_url` | string | RTSP 流地址 |
| `frame_id` | uint64 | 帧序号（全局递增）|
| `timestamp_ms` | int64 | 系统时间戳（毫秒）|
| `pts` | int64 | 原始 PTS（Presentation Time Stamp）|
| `original_width` | int | 原始帧宽度 |
| `original_height` | int | 原始帧高度 |
| `results` | array | 模型推理结果列表 |

**ModelResult 字段**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `task_name` | string | 任务名称 |
| `model_path` | string | 模型路径 |
| `inference_time_ms` | number | 推理耗时（毫秒）|
| `detections` | array | 检测结果列表 |

**Detection 字段**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `class_id` | int | 类别 ID |
| `class_name` | string | 类别名称 |
| `confidence` | float | 置信度（0.0-1.0）|
| `bbox` | object | 检测框（坐标相对于原始帧）|

**BBox 字段**:

| 字段 | 类型 | 说明 |
|-----|------|------|
| `x1` | float | 左上角 x 坐标 |
| `y1` | float | 左上角 y 坐标 |
| `x2` | float | 右下角 x 坐标 |
| `y2` | float | 右下角 y 坐标 |

---

### 7.2 订阅示例

#### Python 示例

```python
import zmq
import json

def subscribe_results(endpoint="tcp://127.0.0.1:5555"):
    """订阅推理结果"""
    context = zmq.Context()
    socket = context.socket(zmq.SUB)
    socket.connect(endpoint)
    socket.setsockopt_string(zmq.SUBSCRIBE, "")  # 订阅所有消息
    
    print(f"Subscribed to {endpoint}")
    
    while True:
        try:
            message = socket.recv_json()
            
            cam_id = message['cam_id']
            frame_id = message['frame_id']
            
            print(f"\n[{cam_id}] Frame {frame_id}")
            
            for result in message['results']:
                task_name = result['task_name']
                detections = result['detections']
                inference_time = result['inference_time_ms']
                
                print(f"  Task: {task_name} ({inference_time:.1f}ms)")
                print(f"  Detected {len(detections)} objects:")
                
                for det in detections:
                    print(f"    - {det['class_name']}: {det['confidence']:.2f} "
                          f"at ({det['bbox']['x1']:.0f}, {det['bbox']['y1']:.0f}, "
                          f"{det['bbox']['x2']:.0f}, {det['bbox']['y2']:.0f})")
        
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"Error: {e}")
    
    socket.close()
    context.term()

if __name__ == "__main__":
    subscribe_results()
```

#### C++ 示例

参见 `tests/zmq_subscriber.cpp`：

```cpp
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <iostream>

int main() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::sub);
    
    socket.connect("tcp://127.0.0.1:5555");
    socket.set(zmq::sockopt::subscribe, "");
    
    std::cout << "Subscribed to infer_server results" << std::endl;
    
    while (true) {
        zmq::message_t message;
        auto res = socket.recv(message, zmq::recv_flags::none);
        
        if (res) {
            std::string msg_str(static_cast<char*>(message.data()), message.size());
            auto json_msg = nlohmann::json::parse(msg_str);
            
            std::cout << "Frame " << json_msg["frame_id"] 
                      << " from " << json_msg["cam_id"] << std::endl;
            
            for (const auto& result : json_msg["results"]) {
                std::cout << "  Task: " << result["task_name"]
                          << " (" << result["inference_time_ms"] << "ms)" << std::endl;
                
                for (const auto& det : result["detections"]) {
                    std::cout << "    - " << det["class_name"]
                              << ": " << det["confidence"] << std::endl;
                }
            }
        }
    }
    
    return 0;
}
```

---

## 8. 完整使用示例

### 8.1 Python 完整示例

```python
import requests
import time
import cv2
import numpy as np

BASE_URL = "http://localhost:8080/api"

def add_stream(cam_id, rtsp_url):
    """添加视频流"""
    url = f"{BASE_URL}/streams"
    data = {
        "cam_id": cam_id,
        "rtsp_url": rtsp_url,
        "frame_skip": 2,
        "models": [
            {
                "model_path": "/models/yolov5_person.rknn",
                "task_name": "person_detection",
                "model_type": "yolov5",
                "conf_threshold": 0.3
            }
        ]
    }
    
    response = requests.post(url, json=data)
    result = response.json()
    
    if result['code'] == 0:
        print(f"✓ Stream {cam_id} added successfully")
    else:
        print(f"✗ Failed to add stream: {result['message']}")
    
    return result

def get_stream_status(cam_id):
    """获取流状态"""
    url = f"{BASE_URL}/streams/{cam_id}"
    response = requests.get(url)
    result = response.json()
    
    if result['code'] == 0:
        status = result['data']
        print(f"\nStream Status: {cam_id}")
        print(f"  Status: {status['status']}")
        print(f"  Decoded: {status['decoded_frames']} frames")
        print(f"  Inferred: {status['inferred_frames']} frames")
        print(f"  Decode FPS: {status['decode_fps']:.1f}")
        print(f"  Infer FPS: {status['infer_fps']:.1f}")
        print(f"  Uptime: {status['uptime_seconds']:.1f}s")
    
    return result

def get_preview(cam_id, output_path="preview.jpg"):
    """获取预览图像"""
    url = f"{BASE_URL}/cache/image"
    params = {"stream_id": cam_id, "latest": "true"}
    
    response = requests.get(url, params=params)
    
    if response.status_code == 200:
        with open(output_path, 'wb') as f:
            f.write(response.content)
        
        frame_id = response.headers.get('X-Frame-Id')
        timestamp = response.headers.get('X-Timestamp-Ms')
        print(f"✓ Preview saved: {output_path} (frame {frame_id})")
        
        # 显示图像
        img = cv2.imread(output_path)
        cv2.imshow("Preview", img)
        cv2.waitKey(1)
    else:
        result = response.json()
        print(f"✗ Failed to get preview: {result['message']}")

def get_server_status():
    """获取服务器状态"""
    url = f"{BASE_URL}/status"
    response = requests.get(url)
    result = response.json()
    
    if result['code'] == 0:
        status = result['data']
        print(f"\nServer Status:")
        print(f"  Version: {status['version']}")
        print(f"  Uptime: {status['uptime_seconds']:.1f}s")
        print(f"  Streams: {status['streams_running']}/{status['streams_total']} running")
        print(f"  Infer Queue: {status['infer_queue_size']} tasks")
        print(f"  Total Processed: {status['infer_total_processed']} frames")
        print(f"  Cache Memory: {status.get('cache_memory_mb', 0):.2f} MB")
    
    return result

def main():
    # 1. 添加视频流
    cam_id = "camera_001"
    rtsp_url = "rtsp://192.168.1.100:554/stream"
    add_stream(cam_id, rtsp_url)
    
    # 2. 等待流启动
    print("\nWaiting for stream to start...")
    time.sleep(3)
    
    # 3. 查询流状态
    get_stream_status(cam_id)
    
    # 4. 查询服务器状态
    get_server_status()
    
    # 5. 持续获取预览图像
    print("\nStreaming preview (press Ctrl+C to stop)...")
    try:
        while True:
            get_preview(cam_id)
            time.sleep(1)  # 每秒刷新一次
    except KeyboardInterrupt:
        print("\nStopped")
    
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
```

### 8.2 Shell 脚本示例

```bash
#!/bin/bash

BASE_URL="http://localhost:8080/api"
CAM_ID="camera_001"
RTSP_URL="rtsp://192.168.1.100:554/stream"

# 1. 添加视频流
echo "Adding stream..."
curl -X POST $BASE_URL/streams \
  -H "Content-Type: application/json" \
  -d "{
    \"cam_id\": \"$CAM_ID\",
    \"rtsp_url\": \"$RTSP_URL\",
    \"frame_skip\": 2,
    \"models\": [
      {
        \"model_path\": \"/models/yolov5_person.rknn\",
        \"task_name\": \"person_detection\",
        \"model_type\": \"yolov5\",
        \"conf_threshold\": 0.3
      }
    ]
  }"

echo -e "\n\nWaiting for stream to start..."
sleep 3

# 2. 查询流状态
echo -e "\n\nGetting stream status..."
curl $BASE_URL/streams/$CAM_ID | jq '.'

# 3. 查询服务器状态
echo -e "\n\nGetting server status..."
curl $BASE_URL/status | jq '.'

# 4. 获取预览图像
echo -e "\n\nGetting preview image..."
curl "$BASE_URL/cache/image?stream_id=$CAM_ID&latest=true" \
  -o preview.jpg

echo -e "\n\nPreview saved to preview.jpg"

# 5. 停止流
echo -e "\n\nStopping stream..."
curl -X POST $BASE_URL/streams/$CAM_ID/stop

# 6. 删除流
echo -e "\n\nRemoving stream..."
curl -X DELETE $BASE_URL/streams/$CAM_ID
```

---

## 附录

### A. 配置文件示例

`config/server.json`:

```json
{
  "http_port": 8080,
  "zmq_endpoint": "tcp://0.0.0.0:5555",
  "num_infer_workers": 3,
  "decode_queue_size": 2,
  "infer_queue_size": 18,
  "streams_save_path": "/etc/infer-server/streams.json",
  "log_level": "info",
  "cache_duration_sec": 5,
  "cache_jpeg_quality": 75,
  "cache_resize_width": 640,
  "cache_resize_height": 0,
  "cache_max_memory_mb": 64
}
```

### B. 标签文件示例

`coco.names`:

```
person
bicycle
car
motorbike
aeroplane
bus
train
truck
...
```

### C. 性能优化建议

1. **推理线程数**: 根据 NPU 核心数设置（RK3588: 2-4 线程）
2. **跳帧策略**: `frame_skip` 设置为 2-5，减少冗余推理
3. **队列大小**: `infer_queue_size = num_infer_workers × 6`
4. **缓存控制**: 
   - 减小 `cache_resize_width` 降低内存占用
   - 减小 `cache_duration_sec` 减少缓存时长
5. **网络优化**: 使用 IPC 而非 TCP 连接 ZeroMQ

### D. 故障排查

#### 流状态为 error

- 检查 `last_error` 字段
- 验证 RTSP URL 是否可访问
- 检查 FFmpeg-RK 是否正确安装

#### 推理队列堆积 (queue_size 持续增大)

- 增加 `num_infer_workers`
- 增大 `frame_skip` 减少推理频率
- 检查模型文件是否正确

#### 图像缓存返回 503

- 检查是否编译了 TurboJPEG 支持
- 确认 `cache_` 相关配置正确

---

**文档版本**: v1.0  
**最后更新**: 2026-02-12  
**维护者**: Infer Server Team
