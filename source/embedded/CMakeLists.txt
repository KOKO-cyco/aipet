##
# @file CMakeLists.txt
# @brief aipet 嵌入式应用顶层 CMake。
#
# 目录结构（新增文件不用改本文件，递归 GLOB + CONFIGURE_DEPENDS 会自动收）：
#   src/                     入口 tuya_app_main.c；递归收集 src/**/*.c
#     core/                  事件总线 / 配置中心 / 页面路由
#     service/               显示 / 音频 / 存储 / 网络 / 时间 / 按键 / 字体 / 天气
#     pages/                 各个页面（每页一个文件）+ 状态栏控件
#     modules/               精灵引擎 / AI 语音对话 / 对话驱动动作 / 扬声器
#     tests/                 宏门控的 bring-up 测试
#   include/                 工程级头文件（tuya_config.h 等）
#   font/                    GUI Guider 生成的中文字体（C 源）
#
# **纯 C 工程**：老产品的 NES 模拟器（QuickNES，C++）和 MP3 软解（minimp3）都
# 随那些模块一起删掉了，所以这里不再 enable_language(CXX)。
#
# 构建：cd source/embedded && tos.py build
#/

########################################
# 源码与头文件
########################################
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

# 递归收集：分层目录（core/service/pages/modules）下的文件都会被编进去，
# 加新文件不用回来改 CMake。CONFIGURE_DEPENDS 让 cmake 自己感知新增文件。
file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS
    ${APP_PATH}/src/*.c
)
file(GLOB APP_FONT_SRC CONFIGURE_DEPENDS
    ${APP_PATH}/font/*.c
)

set(APP_SRC ${APP_SRC} ${APP_FONT_SRC})

# 每个分层都有自己的 include 目录：源文件里直接 #include "app_router.h" 即可
set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/src
    ${APP_PATH}/src/core
    ${APP_PATH}/src/service
    ${APP_PATH}/src/pages
    ${APP_PATH}/src/modules
    ${APP_PATH}/src/tests
    ${APP_PATH}/src/ui
    ${APP_PATH}/src/ui/pui
    ${APP_PATH}/src/ui/fonts
)

########################################
# Armino 构建树里的 sdkconfig.h
#
# app_sprite2d.c 经 SDK 的头文件间接包含 sdkconfig.h，而这个头文件只存在于
# Armino 的**构建目录**里（源码树里没有）。所以要显式把那份 config 目录加进
# include 路径：少了它会报 sdkconfig.h not found。
#
# SDK 根目录由本文件位置推导（source/embedded 往上四级），也允许 -DSDK_ROOT_DIR=...
# 覆盖 —— 早先这里写的是别人机器上的绝对路径，换机器就炸。
########################################
if(NOT DEFINED SDK_ROOT_DIR OR SDK_ROOT_DIR STREQUAL "")
    get_filename_component(SDK_ROOT_DIR "${APP_PATH}/../../../../TuyaOpenSDK" ABSOLUTE)
endif()

set(ARMINO_CFG_DIR
    ${SDK_ROOT_DIR}/platform/T5AI/t5_os/build/bk7258/tuya_app/bk7258_ap/armino_as_lib/bk7258_ap/config)

if(EXISTS ${ARMINO_CFG_DIR})
    list(APPEND APP_INC ${ARMINO_CFG_DIR})
else()
    message(WARNING "Armino config 目录不存在（首次构建前正常）: ${ARMINO_CFG_DIR}")
endif()

# APP_OPTIONS
set(APP_OPTIONS "")  # -W

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
    )
