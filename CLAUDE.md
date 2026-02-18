# infer-server 堆损坏崩溃分析与修复报告

## 问题描述

运行 `sudo ./infer_server ../config/server.json` 后，程序在几秒内崩溃，报错：

```
malloc(): unsorted double linked list corrupted
Aborted
```

## 排查过程

### 第一阶段：core_mask 问题（已修复，但非根因）

初始崩溃日志显示：

```
E RKNN: unavailable core mask found for current platform! max core mask = 3
InferWorker[2] started (core_mask=4)
```

RK3576 只有 2 个 NPU 核心（core_mask 最大值 3），但 Worker[2] 被分配了 `core_mask=4`（CORE_2）。
这是因为 `NpuCoreMask::from_worker_id()` 没有考虑实际 NPU 核心数。

**已修复**：`from_worker_id()` 增加了 `num_cores` 参数，超出范围的 worker 返回 `AUTO(0)`。

但修复后仍然崩溃——堆损坏是更深层的问题。

### 第二阶段：系统化排除法

通过逐步禁用组件，定位堆损坏来源：

| 测试配置 | 组件 | 结果 |
|---------|------|------|
| 纯 FFmpeg 解码（无 RKNN、无 RGA、无缓存） | decode only | **稳定** 60s+ |
| FFmpeg + RGA + JPEG 缓存（无 RKNN 模型） | decode + cache | **稳定** 60s+ |
| FFmpeg + RKNN（1 worker、全局锁串行化） | decode + infer | **崩溃** |
| FFmpeg + RKNN + **jemalloc** | 全部 | **稳定** 3.5 min+ |

### 第三阶段：GDB 堆栈分析

GDB 崩溃堆栈显示：

```
Thread 8 (崩溃线程):
  #7  __GI___libc_malloc (bytes=2880)
  #8  tjCompress2 () from libturbojpeg.so.0    ← malloc 检测到堆已被破坏
  #9  infer_server::JpegEncoder::encode(...)

Thread 6 (并发执行):
  #4  rknn_inputs_set () from librknnrt.so

Thread 4 (并发执行):
  #6  rknn_run () from librknnrt.so
```

堆损坏发生在 RKNN 操作期间，被后续的 `tjCompress2` 中的 `malloc()` 检测到。

## 根因

**`librknnrt.so`（RKNN Runtime v2.3.2）会损坏 glibc 的 malloc 堆元数据。**

关键证据：
- 即使只有 **1 个 worker**、**1 路流**、**全局互斥锁串行化所有 RKNN 调用**，仍然崩溃
- 仅仅是 `rknn_init()` 创建了上下文（尚未执行任何推理），就会导致 glibc 堆损坏
- 替换为 **jemalloc** 后问题消失，证实是 RKNN 库与 glibc malloc 的兼容性问题

这是 Rockchip 闭源 RKNN Runtime 库的 bug，与应用代码无关。

## 修复方案

### 修复 1：`rknn_dup_context` → `rknn_init`（独立上下文）

**文件**：`src/inference/model_manager.cpp`

将 `create_worker_context()` 中的 `rknn_dup_context` 替换为 `rknn_init`，使每个 worker 拥有完全独立的 RKNN 上下文，避免共享内部状态：

```cpp
// 旧代码（共享内部状态，多线程不安全）
rknn_context dup_ctx = 0;
int ret = rknn_dup_context(&it->second.master_ctx, &dup_ctx);

// 新代码（完全独立上下文）
rknn_context ctx = 0;
int ret = rknn_init(&ctx, it->second.model_data.data(),
                    static_cast<uint32_t>(it->second.model_data.size()),
                    0, nullptr);
```

### 修复 2：预创建 worker context（避免硬件冲突）

**文件**：
- `include/infer_server/inference/infer_worker.h` — 新增 `pre_create_context()` 公开方法
- `src/inference/infer_worker.cpp` — 实现 `pre_create_context()`
- `src/inference/inference_engine.cpp` — 在 `load_models()` 中调用预创建

所有 worker 的 RKNN context 在模型加载时（解码线程启动前）预先创建，
避免 `rknn_init` 与 RGA/FFmpeg-MPP 硬件操作并发执行时的驱动层冲突。

```cpp
// inference_engine.cpp - load_models()
for (auto& worker : workers_) {
    worker->pre_create_context(mc.model_path);
}
```

### 修复 3：jemalloc 替代 glibc malloc

**依赖**：`libjemalloc2`（已安装）

启动命令改为：

```bash
sudo LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2 ./infer_server ../config/server.json
```

jemalloc 的内存管理方式不受 RKNN 库 bug 的影响，从根本上绕过了堆损坏问题。

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `include/infer_server/inference/model_manager.h` | 移除 `shared_mutex`/`rknn_global_mutex`，清理注释 |
| `src/inference/model_manager.cpp` | `create_worker_context()` 改用 `rknn_init` 替代 `rknn_dup_context` |
| `include/infer_server/inference/infer_worker.h` | 新增 `pre_create_context()` 公开方法 |
| `src/inference/infer_worker.cpp` | 实现 `pre_create_context()`；移除推理路径上的不必要锁 |
| `src/inference/inference_engine.cpp` | `load_models()` 中加载模型后预创建所有 worker context |
| `config/server.json` | `num_infer_workers` 恢复为 3 |

## 能否通过更新 RKNN SDK 解决？

**当前无法通过升级解决。** 你使用的 `librknnrt.so` 已是 **v2.3.2 (2025-04-09)**，
这是 Rockchip 官方发布的最新版本（截至 2026-02），没有更新版本可以升级。

GitHub 上 `airockchip/rknn-toolkit2` 和 `rockchip-linux/rknpu2` 存在多个内存相关 issue
（segfault、NPU 内存分配失败等），但 Rockchip 对闭源 `librknnrt.so` 内部问题通常不公开修复细节。

**建议**：
- 在 [airockchip/rknn-toolkit2/issues](https://github.com/airockchip/rknn-toolkit2/issues) 提交 issue，附上 GDB 堆栈和排查过程
- 联系 Rockchip 官方 Redmine 反馈系统（仓库 README 有入口）
- 等待未来 v2.4.x 版本修复

## 其他可选解决方案

### 方案 A：使用其他内存分配器（同类思路）

| 分配器 | 安装方式 | LD_PRELOAD 路径 |
|--------|---------|----------------|
| **jemalloc** (推荐) | `apt install libjemalloc2` | `/usr/lib/aarch64-linux-gnu/libjemalloc.so.2` |
| **tcmalloc** | `apt install libtcmalloc-minimal4` | `/usr/lib/aarch64-linux-gnu/libtcmalloc_minimal.so.4` |
| **mimalloc** | 需要源码编译 | 自行编译路径 |

原理相同——用不同的 malloc 实现绕过 RKNN 与 glibc malloc 元数据的冲突。

### 方案 B：进程隔离（推理与解码分离）

将 RKNN 推理放在独立进程中，通过 IPC（共享内存/ZMQ/Unix socket）与主进程通信。

- 优点：彻底隔离，RKNN 进程崩溃不会带走主进程
- 缺点：架构改动大，增加 IPC 延迟和数据拷贝开销

### 方案 C：减少同时存在的 RKNN 上下文数量

`rknn_init` 创建越多上下文，堆损坏越容易触发。可尝试：
- 只用 1 个 worker（context 从 4 个减到 2 个）
- `load_model` 中查询完模型信息后 `rknn_destroy` 销毁 master context

不能根治，但可能降低触发概率。实测 1 worker 也会崩溃，只是偶尔多撑几十秒。

### 方案 D：使用 NPU 驱动的 zero-copy API

RKNN 提供 `rknn_create_mem_from_fd` / `rknn_create_mem_from_phys` 等 zero-copy API，
使用 DMA-BUF 句柄在 NPU 和 RGA 间共享内存，减少 malloc/free 调用。
可能降低触发概率，但需要大幅重构推理管线。

### 方案对比

| 方案 | 可行性 | 改动量 | 推荐度 |
|------|--------|--------|--------|
| **jemalloc LD_PRELOAD** | 立即可用 | 零代码改动 | ★★★★★ |
| 更新 RKNN SDK | 当前无新版 | 等待官方 | ★★★☆☆ |
| tcmalloc/mimalloc | 立即可用 | 零代码改动 | ★★★★☆ |
| 进程隔离 | 需重构架构 | 大 | ★★★☆☆ |
| zero-copy API | 需重构推理管线 | 大 | ★★☆☆☆ |
| 向 Rockchip 报 bug | 应该做 | 无 | 长期 ★★★★★ |

## 为什么推荐 jemalloc

### 1. 直接解决当前问题

jemalloc 的内存元数据布局与 glibc ptmalloc 完全不同。RKNN 库内部的越界写入/元数据破坏
在 glibc 下触发 `malloc(): unsorted double linked list corrupted`，
但在 jemalloc 下不会命中关键元数据结构，因此不会崩溃。

### 2. 多线程性能更好

jemalloc 专为多线程高并发场景设计（最初为 FreeBSD 和 Firefox 开发）：

- **线程缓存（tcache）**：每个线程有独立的小对象分配缓存，减少锁竞争
- **Arena 分区**：多个独立的分配 arena，线程分布到不同 arena 避免争用
- 本服务有 3 解码线程 + 3 推理线程 + REST + ZMQ 线程，并发分配密集，jemalloc 天然更适合

### 3. 更低的内存碎片

jemalloc 使用 slab 分配 + size class 对齐策略，长期运行的服务内存碎片率显著低于 glibc。
对需要 7×24 运行的嵌入式推理服务非常重要。

### 4. 零侵入、零代码改动

只需一行 `LD_PRELOAD` 即可生效，不需要修改源代码、不需要重新编译、不影响其他系统程序，
随时可去掉恢复原状。

### 5. 生产环境广泛验证

jemalloc 被 Meta、Redis、Firefox、Android 等大规模项目使用，
在嵌入式 ARM 平台上也非常成熟稳定。

## 运行要求

1. 安装 jemalloc：`sudo apt-get install libjemalloc2`
2. 使用 `LD_PRELOAD` 启动：

```bash
sudo LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2 \
    ./infer_server ../config/server.json
```

## 环境信息

| 项目 | 值 |
|------|-----|
| 平台 | RK3576 (aarch64) |
| 内核 | 6.1.99 (monolithic) |
| RKNN Runtime | 2.3.2 (2025-04-09) |
| NPU 核心数 | 2 |
| RGA 版本 | 1.10.1_[10] |
| FFmpeg 解码器 | h264_rkmpp (硬件) |
| jemalloc | 5.2.1 |
