# gadget_sim — ESP32-S3 33969 AI Gadget 的 LVGL PC 模拟器（480×480）。
# UI 业务代码在 src/ui/，与固件共享（ADR-001）；数据层在 src/data/。
cmake_minimum_required(VERSION 3.16)
project(gadget_sim C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# CMake 4.x 移除对 <3.5 兼容：cjson 等三方依赖声明过老，设策略下限放行
#（不影响正常构建；CI 全新配置时必需）
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)
FetchContent_Declare(
  lvgl
  URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.2.tar.gz
)
FetchContent_Declare(
  cjson
  URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
)
FetchContent_MakeAvailable(lvgl cjson)

# lv_conf.h 放在本目录，用 LV_CONF_INCLUDE_SIMPLE 让 lvgl 与本工程都读到它。
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

# cJSON 默认开了不少无关选项，裁掉测试/工具
target_compile_options(cjson PRIVATE -Wno-error)

add_executable(gadget_sim
  main.c
  src/data/data_source.c
  src/data/data_source_mock.c
  src/data/data_source_bridge.c
  src/data/data_source_wifi.c
  src/data/data_source_files.c
  src/data/data_source_devstats.c
  src/data/http_client.c
  src/data/mock_data.c
  src/ui/app_manager.c
  src/ui/ui_theme.c
  src/ui/ui_launcher.c
  src/ui/ui_worker.c
  src/ui/ui_app_pages.c
  src/ui/ui_weather.c
  src/ui/ui_conversation.c
  src/ui/companion.c
  src/ui/ui_settings.c
  src/ui/ui_files.c
  src/ui/ui_pomodoro.c
  src/ui/ui_wifi_panel.c
  src/ui/ui_statusbar.c
  src/ui/ui_gesture.c
  src/ui/ui_keys.c
  src/ui/ui_sound.c
  src/voice/voice_session.c
  src/voice/audio_mock.c
  src/ui/fonts/font_cn_12.c
  src/ui/fonts/font_cn_14.c
  src/ui/fonts/font_cn_16.c
  src/ui/fonts/font_cn_20.c
  src/ui/fonts/font_cn_24.c
  src/ui/fonts/font_digits_84.c
  src/ui/fonts/font_digits_64.c
  src/ui/fonts/font_digits_56.c
  src/ui/icons/pomo_mascot.c
  src/ui/fonts/font_pomo_20.c
  src/ui/fonts/font_sub_12.c
  src/ui/fonts/font_sub_14.c
  src/ui/fonts/font_sub_16.c
)
target_include_directories(gadget_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
  ${CMAKE_CURRENT_SOURCE_DIR}/src/data
  ${CMAKE_CURRENT_SOURCE_DIR}/src/voice
  ${cjson_SOURCE_DIR} # cJSON.h 平铺在源码根；INTERFACE 会触发 build 目录前缀错误
)
target_link_libraries(gadget_sim PRIVATE lvgl cjson)
