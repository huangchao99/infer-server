# FindTurboJPEG.cmake
# Find libjpeg-turbo (TurboJPEG API)
#
# Defines:
#   TURBOJPEG_FOUND         - True if found
#   TURBOJPEG_INCLUDE_DIRS  - Include directories
#   TURBOJPEG_LIBRARIES     - Libraries to link
#
# Install: sudo apt install libjpeg-turbo8-dev

find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/libjpeg-turbo/include
)

find_library(TURBOJPEG_LIBRARY turbojpeg
    PATHS
        /usr/lib/aarch64-linux-gnu
        /usr/lib/x86_64-linux-gnu
        /usr/lib
        /usr/local/lib
        /opt/libjpeg-turbo/lib64
        /opt/libjpeg-turbo/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TurboJPEG
    REQUIRED_VARS TURBOJPEG_INCLUDE_DIR TURBOJPEG_LIBRARY
    FAIL_MESSAGE
        "TurboJPEG (libjpeg-turbo) not found. Install with: sudo apt install libjpeg-turbo8-dev"
)

if(TurboJPEG_FOUND)
    set(TURBOJPEG_INCLUDE_DIRS ${TURBOJPEG_INCLUDE_DIR})
    set(TURBOJPEG_LIBRARIES ${TURBOJPEG_LIBRARY})
    message(STATUS "TurboJPEG found:")
    message(STATUS "  Include: ${TURBOJPEG_INCLUDE_DIRS}")
    message(STATUS "  Library: ${TURBOJPEG_LIBRARIES}")
endif()

mark_as_advanced(TURBOJPEG_INCLUDE_DIR TURBOJPEG_LIBRARY)
