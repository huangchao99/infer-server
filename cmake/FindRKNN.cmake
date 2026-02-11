# FindRKNN.cmake
# ===============
# 查找 Rockchip RKNN Runtime 库 (librknnrt)
#
# 用户确认的路径:
#   Header:  /usr/local/include/rknn/rknn_api.h
#   Library: /usr/lib/librknnrt.so
#
# 输出变量:
#   RKNN_FOUND          - 是否找到 RKNN
#   RKNN_INCLUDE_DIRS   - 头文件目录
#   RKNN_LIBRARIES      - 链接库

# 搜索头文件 rknn_api.h
find_path(RKNN_INCLUDE_DIR
    NAMES rknn_api.h
    PATHS
        /usr/local/include/rknn
        /usr/local/include
        /usr/include/rknn
        /usr/include
        ${RKNN_ROOT}/include
    NO_DEFAULT_PATH
)

# 若上面的 NO_DEFAULT_PATH 没找到, 再用默认搜索
if(NOT RKNN_INCLUDE_DIR)
    find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h)
endif()

# 搜索库文件 librknnrt.so
find_library(RKNN_LIBRARY
    NAMES rknnrt
    PATHS
        /usr/lib
        /usr/lib/aarch64-linux-gnu
        /usr/local/lib
        /usr/local/lib/aarch64-linux-gnu
        ${RKNN_ROOT}/lib
    NO_DEFAULT_PATH
)

if(NOT RKNN_LIBRARY)
    find_library(RKNN_LIBRARY NAMES rknnrt)
endif()

# 处理标准参数
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RKNN
    REQUIRED_VARS RKNN_LIBRARY RKNN_INCLUDE_DIR
)

if(RKNN_FOUND)
    set(RKNN_INCLUDE_DIRS ${RKNN_INCLUDE_DIR})
    set(RKNN_LIBRARIES ${RKNN_LIBRARY})

    message(STATUS "Found RKNN:")
    message(STATUS "  Include: ${RKNN_INCLUDE_DIRS}")
    message(STATUS "  Library: ${RKNN_LIBRARIES}")

    # 尝试获取版本信息
    if(EXISTS "${RKNN_LIBRARY}")
        execute_process(
            COMMAND strings "${RKNN_LIBRARY}"
            COMMAND grep "librknnrt version"
            OUTPUT_VARIABLE RKNN_VERSION_STRING
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(RKNN_VERSION_STRING)
            message(STATUS "  Version: ${RKNN_VERSION_STRING}")
        endif()
    endif()
endif()

mark_as_advanced(RKNN_INCLUDE_DIR RKNN_LIBRARY)
