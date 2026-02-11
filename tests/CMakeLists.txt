# ========================
# Test executables
# ========================

# Phase 1: 有界队列单元测试
add_executable(test_bounded_queue test_bounded_queue.cpp)
target_link_libraries(test_bounded_queue PRIVATE infer_server_core)
add_test(NAME test_bounded_queue COMMAND test_bounded_queue)

# Phase 1: 配置管理测试
add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE infer_server_core)
add_test(NAME test_config COMMAND test_config)

# Phase 2: 图片缓存测试 (纯内存, 不需要硬件)
add_executable(test_image_cache test_image_cache.cpp)
target_link_libraries(test_image_cache PRIVATE infer_server_core)
add_test(NAME test_image_cache COMMAND test_image_cache)

# Phase 2: 硬件解码器测试 (需要 ARM 设备 + RTSP 源)
if(ENABLE_FFMPEG)
    add_executable(test_hw_decoder test_hw_decoder.cpp)
    target_link_libraries(test_hw_decoder PRIVATE infer_server_core)
    # 不注册到 CTest (需要 RTSP 参数, 手动运行)
endif()

# Phase 2: RGA 处理器测试 (需要 ARM 设备 + RTSP 源)
if(ENABLE_FFMPEG AND ENABLE_RGA)
    add_executable(test_rga_processor test_rga_processor.cpp)
    target_link_libraries(test_rga_processor PRIVATE infer_server_core)
endif()

# Phase 2: 解码流水线集成测试 (需要全部硬件)
if(ENABLE_FFMPEG AND ENABLE_RGA AND TurboJPEG_FOUND)
    add_executable(test_decode_pipeline test_decode_pipeline.cpp)
    target_link_libraries(test_decode_pipeline PRIVATE infer_server_core)
endif()

# ========================
# Phase 3: 推理与 ZMQ 测试
# ========================

# Phase 3: YOLO 后处理单元测试 (纯 CPU, 不需要硬件)
add_executable(test_post_process test_post_process.cpp)
target_link_libraries(test_post_process PRIVATE infer_server_core)
add_test(NAME test_post_process COMMAND test_post_process)

# Phase 3: 结果聚合器测试 (纯逻辑, 不需要硬件)
add_executable(test_frame_result_collector test_frame_result_collector.cpp)
target_link_libraries(test_frame_result_collector PRIVATE infer_server_core)
add_test(NAME test_frame_result_collector COMMAND test_frame_result_collector)

# Phase 3: ZMQ 发布器测试 (需要 libzmq, 不需要 RKNN 硬件)
if(ENABLE_ZMQ)
    add_executable(test_zmq_publisher test_zmq_publisher.cpp)
    target_link_libraries(test_zmq_publisher PRIVATE infer_server_core)
    add_test(NAME test_zmq_publisher COMMAND test_zmq_publisher)

    # ZMQ 独立订阅者工具 (调试用)
    add_executable(zmq_subscriber zmq_subscriber.cpp)
    target_link_libraries(zmq_subscriber PRIVATE infer_server_core)
endif()

# Phase 3: 模型管理器测试 (需要 RKNN 硬件 + .rknn 模型文件)
if(ENABLE_RKNN)
    add_executable(test_model_manager test_model_manager.cpp)
    target_link_libraries(test_model_manager PRIVATE infer_server_core)
    # 不注册到 CTest (需要模型文件参数, 手动运行)
endif()

# Phase 3: 端到端推理流水线集成测试 (需要全部硬件 + 模型)
if(ENABLE_RKNN AND ENABLE_FFMPEG AND ENABLE_RGA)
    add_executable(test_infer_pipeline test_infer_pipeline.cpp)
    target_link_libraries(test_infer_pipeline PRIVATE infer_server_core)
    # 不注册到 CTest (需要 RTSP + 模型参数, 手动运行)
endif()
