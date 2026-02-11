# FindRGA.cmake
# Find Rockchip RGA (Raster Graphic Acceleration) library
#
# Defines:
#   RGA_FOUND          - True if found
#   RGA_INCLUDE_DIRS   - Include directories
#   RGA_LIBRARIES      - Libraries to link
#
# Search paths:
#   /usr/include/rga/     (standard on RK3588 Ubuntu)
#   /usr/local/include/rga/

# Find headers - look for im2d.h (modern API) and rga.h
find_path(RGA_INCLUDE_DIR
    NAMES im2d.hpp im2d.h rga.h
    PATHS
        /usr/include/rga
        /usr/include
        /usr/local/include/rga
        /usr/local/include
    PATH_SUFFIXES rga
)

# Find library
find_library(RGA_LIBRARY
    NAMES rga
    PATHS
        /usr/lib/aarch64-linux-gnu
        /usr/lib
        /usr/local/lib/aarch64-linux-gnu
        /usr/local/lib
)

# Check for im2d API availability
if(RGA_INCLUDE_DIR)
    if(EXISTS "${RGA_INCLUDE_DIR}/im2d.hpp" OR EXISTS "${RGA_INCLUDE_DIR}/im2d.h")
        set(RGA_HAS_IM2D TRUE)
        message(STATUS "RGA: im2d API available")
    else()
        set(RGA_HAS_IM2D FALSE)
        message(STATUS "RGA: im2d API NOT found, only legacy API available")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RGA
    REQUIRED_VARS RGA_INCLUDE_DIR RGA_LIBRARY
    FAIL_MESSAGE
        "RGA (librga) not found. Install with: sudo apt install librga-dev "
        "or check /usr/include/rga/ and /usr/lib/"
)

if(RGA_FOUND)
    set(RGA_INCLUDE_DIRS ${RGA_INCLUDE_DIR})
    set(RGA_LIBRARIES ${RGA_LIBRARY})
    message(STATUS "RGA found:")
    message(STATUS "  Include: ${RGA_INCLUDE_DIRS}")
    message(STATUS "  Library: ${RGA_LIBRARIES}")
    message(STATUS "  im2d API: ${RGA_HAS_IM2D}")
endif()

mark_as_advanced(RGA_INCLUDE_DIR RGA_LIBRARY)
