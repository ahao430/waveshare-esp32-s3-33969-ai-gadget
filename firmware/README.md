# Firmware（ESP32-S3 固件）

目标板：Waveshare ESP32-S3-Touch-AMOLED-2.16（33969）。
基线：ESP-IDF **v6.0.x**（从 v6.0.2 起步，ADR-004）。

## 构建

```bash
# 安装 ESP-IDF v6.0.2 后：
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

> ⚠️ 当前开发机未安装 ESP-IDF，本目录尚未构建过。
> UI 开发先在 `simulator/lvgl-pc` 进行（ADR-001）。

## 目录规划（PLAN §4.2）

```text
main/           入口 + 系统服务装配
apps/           业务 App（launcher/settings/muse/...，Phase 1+）
components/     系统服务组件（app_manager/audio_manager/voice_session/...）
partitions.csv  16MB 分区表（factory + OTA 双分区 + FAT）
```

## 外设驱动边界（docs/HARDWARE.md §3）

- 显示/触摸/音频/TF：Waveshare BSP `^2.0.1`（`bsp_*` API）。
- AXP2101 / PCF85063 / QMI8658 / 按键：BSP 不提供（`BSP_CAPS_IMU/BUTTONS = 0`），
  Phase 6 在共享 I2C 总线（SDA=15, SCL=14）上自行驱动。
