# FindZMQ.cmake
# ==============
# 查找 ZeroMQ (libzmq) C 库
#
# 常见安装路径 (apt install libzmq3-dev):
#   Header:  /usr/include/zmq.h
#   Library: /usr/lib/aarch64-linux-gnu/libzmq.so
#
# 输出变量:
#   ZMQ_FOUND          - 是否找到 ZeroMQ
#   ZMQ_INCLUDE_DIRS   - 头文件目录
#   ZMQ_LIBRARIES      - 链接库

# 搜索头文件 zmq.h
find_path(ZMQ_INCLUDE_DIR
    NAMES zmq.h
    PATHS
        /usr/include
        /usr/local/include
        ${ZMQ_ROOT}/include
)

# 搜索库文件 libzmq.so
find_library(ZMQ_LIBRARY
    NAMES zmq
    PATHS
        /usr/lib
        /usr/lib/aarch64-linux-gnu
        /usr/local/lib
        /usr/local/lib/aarch64-linux-gnu
        ${ZMQ_ROOT}/lib
)

# 处理标准参数
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZMQ
    REQUIRED_VARS ZMQ_LIBRARY ZMQ_INCLUDE_DIR
)

if(ZMQ_FOUND)
    set(ZMQ_INCLUDE_DIRS ${ZMQ_INCLUDE_DIR})
    set(ZMQ_LIBRARIES ${ZMQ_LIBRARY})

    message(STATUS "Found ZeroMQ:")
    message(STATUS "  Include: ${ZMQ_INCLUDE_DIRS}")
    message(STATUS "  Library: ${ZMQ_LIBRARIES}")

    # 尝试从头文件获取版本
    if(EXISTS "${ZMQ_INCLUDE_DIR}/zmq.h")
        file(STRINGS "${ZMQ_INCLUDE_DIR}/zmq.h" ZMQ_VERSION_MAJOR_LINE
             REGEX "^#define ZMQ_VERSION_MAJOR")
        file(STRINGS "${ZMQ_INCLUDE_DIR}/zmq.h" ZMQ_VERSION_MINOR_LINE
             REGEX "^#define ZMQ_VERSION_MINOR")
        file(STRINGS "${ZMQ_INCLUDE_DIR}/zmq.h" ZMQ_VERSION_PATCH_LINE
             REGEX "^#define ZMQ_VERSION_PATCH")

        if(ZMQ_VERSION_MAJOR_LINE AND ZMQ_VERSION_MINOR_LINE AND ZMQ_VERSION_PATCH_LINE)
            string(REGEX REPLACE ".*#define ZMQ_VERSION_MAJOR[ \t]+([0-9]+).*" "\\1"
                   ZMQ_VERSION_MAJOR "${ZMQ_VERSION_MAJOR_LINE}")
            string(REGEX REPLACE ".*#define ZMQ_VERSION_MINOR[ \t]+([0-9]+).*" "\\1"
                   ZMQ_VERSION_MINOR "${ZMQ_VERSION_MINOR_LINE}")
            string(REGEX REPLACE ".*#define ZMQ_VERSION_PATCH[ \t]+([0-9]+).*" "\\1"
                   ZMQ_VERSION_PATCH "${ZMQ_VERSION_PATCH_LINE}")
            set(ZMQ_VERSION "${ZMQ_VERSION_MAJOR}.${ZMQ_VERSION_MINOR}.${ZMQ_VERSION_PATCH}")
            message(STATUS "  Version: ${ZMQ_VERSION}")
        endif()
    endif()
endif()

mark_as_advanced(ZMQ_INCLUDE_DIR ZMQ_LIBRARY)
