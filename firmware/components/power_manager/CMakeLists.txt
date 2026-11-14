# power_manager — 电池/电源管理组件（PLAN §13.3）
# 依赖：esp_driver_i2c（共享 I²C 总线句柄由 BSP 传入）；不带 BSP 硬依赖，
# 便于独立单测与后续替换 PMU。
idf_component_register(SRCS "src/power_manager.c" "src/battery_axp2101.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_timer)
