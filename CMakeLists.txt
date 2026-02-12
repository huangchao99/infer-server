cmake_minimum_required(VERSION 3.16)
project(infer_server VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ========================
# Build type
# ========================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# ========================
# Options
# ========================
option(BUILD_TESTS  "Build test programs"               ON)
option(ENABLE_FFMPEG "Enable FFmpeg hardware decoding"   ON)
option(ENABLE_RGA    "Enable RGA hardware processing"    ON)

# Phase 3 options
option(ENABLE_RKNN  "Enable RKNN NPU inference"         ON)
option(ENABLE_ZMQ   "Enable ZeroMQ output"              ON)

# Phase 4 options
option(ENABLE_HTTP  "Enable HTTP REST API server"        ON)

# ========================
# CMake module path
# ========================
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ========================
# Third-party dependencies (FetchContent)
# ========================
include(FetchContent)

# nlohmann/json (header-only JSON library)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    # DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# spdlog (fast logging library)
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.13.0.tar.gz
    # DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

message(STATUS "Fetching third-party dependencies (nlohmann/json, spdlog)...")
FetchContent_MakeAvailable(json spdlog)

# cpp-httplib (header-only HTTP server/client library)
if(ENABLE_HTTP)
    FetchContent_Declare(
        httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    )
    set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
    FetchContent_Populate(httplib)
    set(HTTPLIB_INCLUDE_DIR ${httplib_SOURCE_DIR})
    message(STATUS "cpp-httplib header dir: ${HTTPLIB_INCLUDE_DIR}")
endif()

# cppzmq (header-only C++ wrapper for ZeroMQ)
if(ENABLE_ZMQ)
    FetchContent_Declare(
        cppzmq
        URL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz
    )
    # 不让 cppzmq 自己去 find libzmq (我们自己找)
    set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Populate(cppzmq)
    # cppzmq 是 header-only, 只需要 include 路径
    set(CPPZMQ_INCLUDE_DIR ${cppzmq_SOURCE_DIR})
    message(STATUS "cppzmq header dir: ${CPPZMQ_INCLUDE_DIR}")
endif()

# ========================
# System dependencies
# ========================

# TurboJPEG (libjpeg-turbo) - for image cache JPEG encoding
find_package(TurboJPEG)

# FFmpeg-RK (Rockchip-patched FFmpeg) - for hardware decoding
if(ENABLE_FFMPEG)
    find_package(FFmpegRK)
    if(NOT FFmpegRK_FOUND)
        message(WARNING "FFmpeg-RK not found, hardware decoding disabled. "
                        "Set FFMPEG_RK_ROOT or install FFmpeg at /opt/ffmpeg-rk")
        set(ENABLE_FFMPEG OFF)
    endif()
endif()

# RGA (Rockchip Raster Graphic Acceleration) - for hardware resize/convert
if(ENABLE_RGA)
    find_package(RGA)
    if(NOT RGA_FOUND)
        message(WARNING "RGA not found, hardware image processing disabled.")
        set(ENABLE_RGA OFF)
    endif()
endif()

# RKNN (Rockchip NPU Runtime) - for NPU inference
if(ENABLE_RKNN)
    find_package(RKNN)
    if(NOT RKNN_FOUND)
        message(WARNING "RKNN not found, NPU inference disabled. "
                        "Ensure librknnrt is installed or set RKNN_ROOT.")
        set(ENABLE_RKNN OFF)
    endif()
endif()

# ZeroMQ (libzmq) - for result publishing
if(ENABLE_ZMQ)
    find_package(ZMQ)
    if(NOT ZMQ_FOUND)
        message(WARNING "ZeroMQ not found, ZMQ output disabled. "
                        "Install: sudo apt install libzmq3-dev")
        set(ENABLE_ZMQ OFF)
    endif()
endif()

# ========================
# Core library
# ========================
set(CORE_SOURCES
    src/common/config.cpp
    src/common/logger.cpp
)

# Image cache (needs TurboJPEG)
if(TurboJPEG_FOUND)
    list(APPEND CORE_SOURCES
        src/cache/jpeg_encoder.cpp
        src/cache/image_cache.cpp
    )
endif()

# Hardware decoder (needs FFmpeg-RK)
if(ENABLE_FFMPEG)
    list(APPEND CORE_SOURCES
        src/decoder/hw_decoder.cpp
    )
endif()

# RGA processor (needs librga)
if(ENABLE_RGA)
    list(APPEND CORE_SOURCES
        src/processor/rga_processor.cpp
    )
endif()

# RKNN inference (needs librknnrt)
if(ENABLE_RKNN)
    list(APPEND CORE_SOURCES
        src/inference/model_manager.cpp
        src/inference/infer_worker.cpp
        src/inference/inference_engine.cpp
    )
endif()

# Post-processor (纯 CPU 计算, 不依赖硬件库)
list(APPEND CORE_SOURCES
    src/inference/post_processor.cpp
)

# ZeroMQ publisher (needs libzmq)
if(ENABLE_ZMQ)
    list(APPEND CORE_SOURCES
        src/output/zmq_publisher.cpp
    )
endif()

# StreamManager (流生命周期管理, 条件引用硬件组件)
list(APPEND CORE_SOURCES
    src/stream/stream_manager.cpp
)

# REST API server (needs cpp-httplib)
if(ENABLE_HTTP)
    list(APPEND CORE_SOURCES
        src/api/rest_server.cpp
    )
endif()

add_library(infer_server_core STATIC ${CORE_SOURCES})

target_include_directories(infer_server_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(infer_server_core PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    pthread
)

# Ensure filesystem support (GCC < 9 needs stdc++fs)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(infer_server_core PUBLIC stdc++fs)
endif()

# TurboJPEG
if(TurboJPEG_FOUND)
    target_include_directories(infer_server_core PUBLIC ${TURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(infer_server_core PUBLIC ${TURBOJPEG_LIBRARIES})
    target_compile_definitions(infer_server_core PUBLIC HAS_TURBOJPEG=1)
endif()

# FFmpeg-RK
if(ENABLE_FFMPEG)
    target_include_directories(infer_server_core PUBLIC ${FFMPEG_RK_INCLUDE_DIRS})
    target_link_libraries(infer_server_core PUBLIC ${FFMPEG_RK_LIBRARIES})
    target_compile_definitions(infer_server_core PUBLIC HAS_FFMPEG=1)
    # Add rpath so the binary can find FFmpeg libs at runtime
    target_link_directories(infer_server_core PUBLIC ${FFMPEG_RK_LIBRARY_DIR})
    set(CMAKE_INSTALL_RPATH "${FFMPEG_RK_LIBRARY_DIR}")
    set(CMAKE_BUILD_RPATH "${FFMPEG_RK_LIBRARY_DIR}")
endif()

# RGA
if(ENABLE_RGA)
    target_include_directories(infer_server_core PUBLIC ${RGA_INCLUDE_DIRS})
    target_link_libraries(infer_server_core PUBLIC ${RGA_LIBRARIES})
    target_compile_definitions(infer_server_core PUBLIC HAS_RGA=1)
endif()

# RKNN
if(ENABLE_RKNN)
    target_include_directories(infer_server_core PUBLIC ${RKNN_INCLUDE_DIRS})
    target_link_libraries(infer_server_core PUBLIC ${RKNN_LIBRARIES})
    target_compile_definitions(infer_server_core PUBLIC HAS_RKNN=1)
endif()

# ZeroMQ
if(ENABLE_ZMQ)
    target_include_directories(infer_server_core PUBLIC
        ${ZMQ_INCLUDE_DIRS}
        ${CPPZMQ_INCLUDE_DIR}
    )
    target_link_libraries(infer_server_core PUBLIC ${ZMQ_LIBRARIES})
    target_compile_definitions(infer_server_core PUBLIC HAS_ZMQ=1)
endif()

# cpp-httplib
if(ENABLE_HTTP)
    target_include_directories(infer_server_core PUBLIC ${HTTPLIB_INCLUDE_DIR})
    target_compile_definitions(infer_server_core PUBLIC HAS_HTTP=1)
endif()

# ========================
# Main executable
# ========================
add_executable(infer_server src/main.cpp)
target_link_libraries(infer_server PRIVATE infer_server_core)

# ========================
# Tests
# ========================
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    message(STATUS "Tests enabled")
endif()

# ========================
# Install
# ========================
install(TARGETS infer_server DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

# ========================
# Summary
# ========================
message(STATUS "")
message(STATUS "=== Infer Server Build Configuration ===")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests:        ${BUILD_TESTS}")
message(STATUS "  FFmpeg-RK:    ${ENABLE_FFMPEG}")
message(STATUS "  RGA:          ${ENABLE_RGA}")
message(STATUS "  TurboJPEG:    ${TurboJPEG_FOUND}")
message(STATUS "  RKNN:         ${ENABLE_RKNN}")
message(STATUS "  ZeroMQ:       ${ENABLE_ZMQ}")
message(STATUS "  HTTP API:     ${ENABLE_HTTP}")
message(STATUS "========================================")
