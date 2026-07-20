cmake_minimum_required(VERSION 3.16)

project(v4l2_basic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 生成 compile_commands.json 以供 clangd 使用

# Output layout: build/bin for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(app
    src/main.cpp
    src/rkmppdec.cpp
    src/rkmppenc.cpp
    src/zlm_publisher.cpp
    src/rkrga.cpp
    src/yolo_npu.cpp
    src/npu_detector_pool.cpp
    src/app_controller.cpp
    src/nv12_overlay.cpp
    src/v4l2Camera.cpp
    # Examples: add more source files here
    # src/utils/log.cpp
)

# Use include/ for public headers + RK MPP/RGA headers
# RK 平台常见头文件路径：/usr/include/rockchip, /usr/include/rga
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ZLM
    /usr/include/rockchip
    /usr/include/rga
)

# ARM Optimization Flags for RK3588 (ARMv8.2-A)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE
        -Wall -Wextra -Wpedantic
        -O3
        -march=armv8.2-a+fp16+dotprod+crypto
        -mtune=cortex-a76
        -ffast-math
        -flto=auto
    )
    target_compile_options(app PRIVATE
        $<$<CONFIG:Release>:-DNDEBUG>
    )
endif()

# Enable LTO/IPO for release builds
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
if(LTO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

find_package(Threads REQUIRED)

# 显式查找 Rockchip MPP / RGA 动态库，便于后续接入解码与图像处理链路
find_path(RGA_INCLUDE_DIR
    NAMES im2d.h
    PATHS /usr/include/rga
    NO_DEFAULT_PATH
)
if (NOT RGA_INCLUDE_DIR)
    message(FATAL_ERROR "Failed to find RGA headers (im2d.h) in /usr/include/rga")
endif()

target_include_directories(app PRIVATE ${RGA_INCLUDE_DIR})

find_library(ROCKCHIP_MPP_LIB NAMES rockchip_mpp REQUIRED)
find_library(RGA_LIB NAMES rga REQUIRED)
find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h PATHS /usr/include /usr/include/rknn)
if (NOT RKNN_INCLUDE_DIR)
    message(FATAL_ERROR "Failed to find RKNN headers (rknn_api.h)")
endif()
find_library(RKNNRT_LIB NAMES rknnrt REQUIRED)
target_include_directories(app PRIVATE ${RKNN_INCLUDE_DIR})

# 本地 ZLMediaKit C API 动态库及头文件
set(ZLM_LOCAL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ZLM)
set(ZLM_LOCAL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

if (NOT EXISTS ${ZLM_LOCAL_INCLUDE_DIR}/mk_mediakit.h)
    message(FATAL_ERROR "Failed to find ZLM C API headers in ${ZLM_LOCAL_INCLUDE_DIR}")
endif()

find_library(ZLM_MK_API_LIB
    NAMES mk_api
    PATHS ${ZLM_LOCAL_LIB_DIR}
    NO_DEFAULT_PATH
)
if (NOT ZLM_MK_API_LIB)
    message(FATAL_ERROR "Failed to find libmk_api.so in ${ZLM_LOCAL_LIB_DIR}")
endif()

find_library(ZLM_TOOLKIT_LIB
    NAMES ZLToolKit
    PATHS ${ZLM_LOCAL_LIB_DIR}
    NO_DEFAULT_PATH
)
if (NOT ZLM_TOOLKIT_LIB)
    message(FATAL_ERROR "Failed to find libZLToolKit.so in ${ZLM_LOCAL_LIB_DIR}")
endif()

target_link_libraries(app PRIVATE
    Threads::Threads
    v4l2
    ${ROCKCHIP_MPP_LIB}
    ${RGA_LIB}
    ${RKNNRT_LIB}
    ${ZLM_TOOLKIT_LIB}
    ${ZLM_MK_API_LIB}
)

# 运行时优先从项目根目录下的 lib/ 加载动态库
set_target_properties(app PROPERTIES
    BUILD_RPATH ${ZLM_LOCAL_LIB_DIR}
)

# 使用 DT_RPATH（而不是 DT_RUNPATH），让可执行文件的 rpath 也参与二级依赖解析。
target_link_options(app PRIVATE
    -Wl,--disable-new-dtags
    -Wl,--no-as-needed
    -Wl,-u,Assert_Throw
)

# Enable Unity Build for faster compilation
set(CMAKE_UNITY_BUILD ON)
set(CMAKE_UNITY_BUILD_BATCH_SIZE 16)

# Parallel build
if(NOT DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
    include(ProcessorCount)
    ProcessorCount(NCPUS)
    if(NCPUS GREATER 0)
        set(CMAKE_BUILD_PARALLEL_LEVEL ${NCPUS})
        set(CMAKE_INSTALL_PARALLEL_LEVEL ${NCPUS})
    endif()
endif()
