idf_component_register(SRCS "main.c" INCLUDE_DIRS ".")

# Phase 1 起，把模拟器共享 UI 源编入固件（ADR-001）：
# esp_add_component_source? 届时直接把 simulator/lvgl-pc/src/ui/*.c 追加到 SRCS。
