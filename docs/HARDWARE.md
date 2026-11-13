# 硬件调研：Waveshare ESP32-S3-Touch-AMOLED-2.16（SKU 33969）

> 信息等级说明：
> - ✅ **已确认**：来自官方仓库/Wiki/BSP 源码/上游 SDK 文档，可直接使用。
> - ⚠️ **待验证**：来自社区或上游经验，真机到手后必须复核。
> - ❓ **缺失**：官方资料未提供，只能真机实测。
>
> 调研日期：2026-11-14。来源见文末。

## 1. 板卡概述

| 项目 | 规格 | 等级 |
|------|------|------|
| MCU | ESP32-S3R8（双核 Xtensa LX7 @240MHz，8MB PSRAM，16MB Flash） | ✅ |
| 显示 | 2.16" AMOLED 480×480，CO5300 控制器，QSPI 接口，RGB565 | ✅ |
| 触摸 | CST9220（由 CST9217 驱动驱动，上报 chip type 0x9220），单点 | ✅ |
| 音频输出 | ES8311 codec + 功放（PA 5.0V，DAC 3.3V） | ✅ |
| 音频输入 | ES7210 ADC，双麦克风 | ✅ |
| PMU | AXP2101 | ✅ |
| IMU | QMI8658 | ✅ |
| RTC | PCF85063 | ✅ |
| TF 卡 | MicroSD，SPI/1-bit SDIO（BSP 按 1-bit 模式配置，D1–D7 = NC） | ✅ |
| 电池 | 3.7V 锂电，MX1.25 2P 接口，支持充放电；**默认容量官方未标注** | ❓ |
| 板载按键 | BOOT(GPIO0)、KEY3/GPIO18、PWRON（PMU 电源键，非 GPIO） | ✅ |

## 2. 引脚表

### 2.1 共享 I2C 总线（挂：触摸/ES8311/ES7210/AXP2101/PCF85063/QMI8658）

| 信号 | GPIO | 等级 |
|------|------|------|
| I2C_SCL | 14 | ✅ BSP `BSP_I2C_SCL` |
| I2C_SDA | 15 | ✅ BSP `BSP_I2C_SDA` |

### 2.2 显示（CO5300，QSPI，SPI2_HOST）

| 信号 | GPIO | 等级 |
|------|------|------|
| QSPI_SCLK (PCLK) | 38 | ✅ |
| QSPI_SIO0/1/2/3 (DATA0–3) | 4 / 5 / 6 / 7 | ✅ |
| LCD_CS | 12 | ✅ |
| LCD_RST | 39 | ✅ |
| 背光 | 无独立引脚（AMOLED，亮度走面板命令 0x51） | ✅ |

显示要点（来自 Muse 移植文档，⚠️ 实现时复核）：

- CO5300 更新窗口需**偶数对齐**。
- 亮度 = 面板命令 `0x51`；睡眠/唤醒 = `SLPIN/SLPOUT`（0x10/0x11）。
- 旋转 MADCTL(0x36)：0°=0x00，90°=0x60，180°=0xC0，270°=0xA0。
- Waveshare BSP 的 `bsp_display_start()` 默认 LVGL 缓冲放 **PSRAM**
  （buffer_height=50 双缓冲）；Muse 移植发现 **Wi-Fi/BLE 启动后从
  PSRAM 绘制会失败**，改用两块内部 RAM 分行缓冲（`muse_lcd_bands.c`）。
  本项目显示缓冲策略必须真机验证（PLAN §14.2）。

### 2.3 触摸（CST9220/CST9217）

| 信号 | GPIO | 等级 |
|------|------|------|
| TP_INT | 11 | ✅ |
| TP_RST | 40 | ✅ |

- BSP 通过 `ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG()` 使用 cst9217
  组件默认地址。⚠️ 地址具体数值（常见 0x5A/0x5B）以组件源码/i2cdetect 为准。

### 2.4 音频（I2S）

| 信号 | GPIO | 等级 |
|------|------|------|
| I2S_MCLK | 42 | ✅ |
| I2S_SCLK / BCLK | 9 | ✅ |
| I2S_LCLK / LRCK / WS | 45 | ✅ |
| I2S_DSDIN（ES8311 播放数据 → 喇叭） | 8 | ✅ |
| I2S_ASDOUT（ES7210 麦克风数据 ← MIC） | 10 | ✅ |
| PA_CTRL（功放使能） | 46 | ✅ |

- BSP 默认音频参数：22050 Hz / 16-bit / 单声道 / Philips 标准 I2S。
- Muse 移植自测：扬声器播放通过；ES7210 采集通过。
- ES8311 地址 = `ES8311_CODEC_DEFAULT_ADDR`，ES7210 = 
  `ES7210_CODEC_DEFAULT_ADDR`（esp_codec_dev 组件默认值；⚠️ 常见
  0x18 / 0x40，以组件源码为准）。

### 2.5 TF 卡（SPI，1-bit 模式）

| 信号 | GPIO | 等级 |
|------|------|------|
| SD_CMD / MOSI | 1 | ✅ |
| SD_CLK | 2 | ✅ |
| SD_D0 / MISO | 3 | ✅ |
| SD_CS | 41 | ✅（Wiki 引脚图） |
| SD_D1–D7 | NC（BSP 1-bit 模式） | ✅ |

### 2.6 传感器 / 电源 / 按键 / 其他

| 信号 | GPIO | 等级 |
|------|------|------|
| QMI8658 INT1 | 17 | ✅ |
| QMI8658 INT2 | 21 | ✅ |
| RTC_INT（PCF85063） | 13 | ✅ |
| BOOT 键 | 0 | ✅ |
| KEY3（用户键，上拉 3V3，按下接地） | 18 | ✅ |
| SYS_OUT | 16 | ✅（电源系统信号，用途待原理图复核） |
| PWRON | PMU 按键输入 | ✅ |

**I2C 从机地址清单**（除 QMI8658 外均需真机 i2cdetect 确认）：

| 芯片 | 地址 | 等级 |
|------|------|------|
| QMI8658 | 0x6B | ✅（官方 components.md 引原理图） |
| AXP2101 | 0x34（常见默认） | ⚠️ 待验证 |
| PCF85063 | 0x51（常见默认） | ⚠️ 待验证 |
| ES8311 | esp_codec_dev 默认（常见 0x18） | ⚠️ 待验证 |
| ES7210 | esp_codec_dev 默认（常见 0x40，AD 引脚可变） | ⚠️ 待验证 |
| CST9220/9217 | cst9217 组件默认（常见 0x5A） | ⚠️ 待验证 |

## 3. BSP 与上游版本

| 上游 | 版本 | 要点 | 等级 |
|------|------|------|------|
| Waveshare BSP | `waveshare/esp32_s3_touch_amoled_2_16` **^2.0.1**（硬件 V1.0 对应 BSP ^1，共 3 版） | 组件注册表；源码在 `waveshareteam/Waveshare-ESP32-components/bsp/esp32_s3_touch_amoled_2_16` | ✅ |
| Waveshare 官方示例 | ESP-IDF **v5.5.5 / v6.0.2** 测试通过 | 仓库 README | ✅ |
| BSP 能力宏 | `BSP_CAPS_DISPLAY/TOUCH/AUDIO/AUDIO_SPEAKER/AUDIO_MIC/SDCARD = 1`；**`BSP_CAPS_IMU = 0`、`BSP_CAPS_BUTTONS = 0`** | IMU/RTC/按键/PMU 需自行驱动（BSP 提供 `bsp_i2c_init/get_handle` 可复用总线） | ✅ |
| BSP 关键 API | `bsp_i2c_init/deinit/get_handle`、`bsp_display_start(_with_config)/new`、`bsp_touch_new`、`bsp_display_brightness_set`、`bsp_audio_init`、`bsp_audio_codec_speaker_init`、`bsp_audio_codec_microphone_init`、`bsp_sdcard_mount/unmount`、`bsp_spiffs_mount/unmount` | 源码提取 | ✅ |
| Muse Gadget SDK | ESP-IDF **v6.0.3**（其他版本不支持） | **已官方支持 33969**：`sdkconfig.muse-waveshare-s3-216`（含 power-test 变体），支持头像 UI/push-to-talk(KEY3)/触摸设置/图片显示/BLE 配对/固件隧道 | ✅ |
| xiaozhi-esp32 | ESP-IDF **≥ v6.0.1**，推荐 `release/v6.1`（5.x 不再支持；勿用 v6.1 release tag，有 C3 Wi-Fi bug） | **无 33969 板级支持**（只见到 AMOLED-1.8）；自定义板指南 `docs/custom-board.md` | ✅ |

### Muse 移植已知的板级坑（来自上游文档，真机复核）

1. 内部 RAM 紧张：Wi-Fi 加入后最低约 **6 KB** 空闲，隧道建立后 14–35 KB。
2. **AXP2101 IRQ 未接到 ESP32** → PWR 键无法从 light sleep 唤醒（Muse 用
   KEY3/GPIO18 做唤醒键）；本项目电源管理设计必须照此假设。
3. 运行中长按 PWR 不关机（固件关了 PMU 关机保持），固件停转后长按 10s 断电。
4. 触摸仅单点，LVGL 手势识别默认关闭——Launcher 手势交互要按单点设计或
   自行开启手势库并验证。
5. IMU/RTC/TF 在 Muse 固件中未集成，无参考实现。

## 4. 待真机验证清单（Phase 6）

- [ ] i2cdetect 扫描全部 I2C 地址，确认 2.6 节 ⚠️ 项。
- [ ] 电池默认容量与 AXP2101 电池参数（充电电流/截止电压）。
- [ ] PSRAM 显示双缓冲在 Wi-Fi+BLE+音频并发下是否稳定（对照 Muse 坑 1）。
- [ ] KEY3(GPIO18) 触摸/按键唤醒 light sleep 的实际能力。
- [ ] TF 卡 1-bit 模式实际吞吐（音乐播放是否够用，是否需 4-bit）。
- [ ] CST9220 是否支持多点与手势（Muse 记录为单点）。
- [ ] 亮屏/息屏/联网/代理场景电流与续航（PLAN §14.3）。
- [ ] `SYS_OUT`(GPIO16) 具体功能（对照原理图）。
- [ ] BSP 头文件注释提到 QMA7981/OV2640——疑似模板残留，本板应为
      QMI8658、无摄像头，以原理图为准。

## 5. 调研来源

- Waveshare 官方仓库：<https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16>（README、docs/brookesia.md、docs/components.md）
- Waveshare Wiki：<https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16>
- BSP 源码：<https://github.com/waveshareteam/Waveshare-ESP32-components/tree/main/bsp/esp32_s3_touch_amoled_2_16>
- BSP 组件页：<https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_16>
- Muse Gadget SDK：<https://github.com/facebookincubator/muse-gadget-sdk>（esp32/README.md、devices/waveshare-s3-216.md、devices/ 目录）
- xiaozhi-esp32：<https://github.com/78/xiaozhi-esp32>（README.md）
