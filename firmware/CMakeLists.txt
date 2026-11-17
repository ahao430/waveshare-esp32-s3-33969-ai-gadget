# AI Gadget 固件（ESP32-S3，目标板 Waveshare 33969）
# 基线 ESP-IDF v6.0.x（ADR-004）；本机未安装 IDF 时无法构建，见 docs/DEVELOPMENT.md。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_gadget VERSION 1.0.0)
