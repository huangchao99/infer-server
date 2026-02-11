# FindFFmpegRK.cmake
# Find Rockchip-patched FFmpeg installed at /opt/ffmpeg-rk
#
# Defines:
#   FFMPEG_RK_FOUND          - True if all required components found
#   FFMPEG_RK_INCLUDE_DIRS   - Include directories
#   FFMPEG_RK_LIBRARIES      - Libraries to link
#
# User can set FFMPEG_RK_ROOT to override the search path

set(FFMPEG_RK_ROOT "/opt/ffmpeg-rk" CACHE PATH "FFmpeg-RK installation root directory")

# --- avformat ---
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h
    PATHS ${FFMPEG_RK_ROOT}/include
    NO_DEFAULT_PATH
)
find_library(AVFORMAT_LIBRARY avformat
    PATHS ${FFMPEG_RK_ROOT}/lib
    NO_DEFAULT_PATH
)

# --- avcodec ---
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
    PATHS ${FFMPEG_RK_ROOT}/include
    NO_DEFAULT_PATH
)
find_library(AVCODEC_LIBRARY avcodec
    PATHS ${FFMPEG_RK_ROOT}/lib
    NO_DEFAULT_PATH
)

# --- avutil ---
find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h
    PATHS ${FFMPEG_RK_ROOT}/include
    NO_DEFAULT_PATH
)
find_library(AVUTIL_LIBRARY avutil
    PATHS ${FFMPEG_RK_ROOT}/lib
    NO_DEFAULT_PATH
)

# --- swscale (optional but useful) ---
find_library(SWSCALE_LIBRARY swscale
    PATHS ${FFMPEG_RK_ROOT}/lib
    NO_DEFAULT_PATH
)

# --- swresample (may be needed as transitive dep) ---
find_library(SWRESAMPLE_LIBRARY swresample
    PATHS ${FFMPEG_RK_ROOT}/lib
    NO_DEFAULT_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpegRK
    REQUIRED_VARS
        AVFORMAT_INCLUDE_DIR AVFORMAT_LIBRARY
        AVCODEC_INCLUDE_DIR  AVCODEC_LIBRARY
        AVUTIL_INCLUDE_DIR   AVUTIL_LIBRARY
    FAIL_MESSAGE
        "FFmpeg-RK not found at ${FFMPEG_RK_ROOT}. Set FFMPEG_RK_ROOT to the FFmpeg installation directory."
)

if(FFmpegRK_FOUND)
    set(FFMPEG_RK_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
    set(FFMPEG_RK_LIBRARIES
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
    )
    if(SWSCALE_LIBRARY)
        list(APPEND FFMPEG_RK_LIBRARIES ${SWSCALE_LIBRARY})
    endif()
    if(SWRESAMPLE_LIBRARY)
        list(APPEND FFMPEG_RK_LIBRARIES ${SWRESAMPLE_LIBRARY})
    endif()

    # FFmpeg 静态库的系统依赖
    # 这些库是 FFmpeg 静态库需要的传递依赖，必须手动指定
    list(APPEND FFMPEG_RK_LIBRARIES
        z                # zlib - 用于压缩/解压 (SWF, CSCD, EXR, Flash 等格式)
        drm              # libdrm - DRM 硬件上下文
        rockchip_mpp     # librockchip_mpp - Rockchip MPP 硬件加速
    )

    # Runtime library path (needed since FFmpeg is in a non-standard location)
    set(FFMPEG_RK_LIBRARY_DIR "${FFMPEG_RK_ROOT}/lib")

    message(STATUS "FFmpeg-RK found at: ${FFMPEG_RK_ROOT}")
    message(STATUS "  Include: ${FFMPEG_RK_INCLUDE_DIRS}")
    message(STATUS "  Libraries: ${FFMPEG_RK_LIBRARIES}")
endif()

mark_as_advanced(
    AVFORMAT_INCLUDE_DIR AVFORMAT_LIBRARY
    AVCODEC_INCLUDE_DIR  AVCODEC_LIBRARY
    AVUTIL_INCLUDE_DIR   AVUTIL_LIBRARY
    SWSCALE_LIBRARY SWRESAMPLE_LIBRARY
)
