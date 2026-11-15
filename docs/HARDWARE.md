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

**I2C 从机地址清单**（2026-11-14 真机首验：骨架固件全地址扫描 0x08–0x77，
六颗全中、无多余设备，日志见 §2.7）：

| 芯片 | 地址 | 等级 |
|------|------|------|
| QMI8658 | 0x6B | ✅（真机 ACK） |
| AXP2101 | 0x34 | ✅（真机 ACK，reg00 读出 0x38） |
| PCF85063 | 0x51 | ✅（真机 ACK） |
| ES8311 | 0x18 | ✅（真机 ACK） |
| ES7210 | 0x40 | ✅（真机 ACK） |
| CST9220/9217 | 0x5A | ✅（真机 ACK；驱动识别 ChipType 0x9220 / ProjectID 0x542F / 480×480） |

### 2.7 真机首验记录（2026-11-14，Phase 6 bring-up）

| 项目 | 结果 |
|------|------|
| 芯片 | ESP32-S3 (QFN56) rev v0.2，MAC 44:bd:8d:79:ba:c4，USB-Serial/JTAG |
| Flash | 16MB（Micron 系 JEDEC 20/4018，quad，3.3V） |
| PSRAM | 内嵌 8MB（AP_3v3，octal——esptool chip-id 报告） |
| I2C 扫描 | 0x08–0x77 全扫：0x18/0x34/0x40/0x51/0x5A/0x6B 全 ACK，无未知设备 |
| 显示 | CO5300 QSPI 面板 v2.2.0 驱动点亮，LVGL 任务 + 背光正常 |
| 触摸 | cst9217 组件识别 CST9220（IRQ 模式），480×480 |
| 原厂备份 | `~/gadget-factory-backup-20261114.bin`（16MB 全片，sha256 见同目录 .sha256） |

**原厂分区表**（从备份 0x8000 解析；本项目刷入后会替换为 `firmware/partitions.csv`，
还原原厂时按此表 write-flash 回去）：

| 分区 | 类型 | 偏移 | 大小 | 用途 |
|------|------|------|------|------|
| nvsfactory | data/nvs | 0x9000 | 0x32000 | 出厂 NVS |
| nvs | data/nvs | 0x3b000 | 0xd2000 | 用户 NVS |
| otadata | data/ota | 0x10d000 | 0x2000 | OTA 指针 |
| phy_init | data/coredump | 0x10f000 | 0x1000 | （subtype 异常，原厂如此） |
| model | data/efuse | 0x110000 | 0xf0000 | 原厂模型数据 |
| factory | app/factory | 0x200000 | 0x800000 | 原厂应用（8MB） |
| storage | data/efuse | 0xa00000 | 0x600000 | 原厂用户存储 |

**bring-up 修过的坑**：
- `esp_timer_start_periodic` 周期单位是 **µs**，power_manager 按 ms 传入导致
  电池轮询 30s→30ms 刷屏（已修，×1000）。
- **显示管线全屏重绘堵死**（2026-11-14，三次复现）：Wi-Fi 开启态全屏
  480×480 @4fps 连续重绘 30s，结束后 LVGL 任务挂起（等 flush 完成）、面板
  白屏；芯片/Wi-Fi/音频/中断均正常（KEY3 触发音、ping 通），断电重启恢复。
  esp_lv_adapter TEAR_AVOID_NONE + PSRAM 帧缓冲路径。**UI 移植约束：
  局部失效 + 节流，全屏刷新仅页面切换低频使用**（同 Muse 坑 #1 机理）。
- GOT_IP 等跨任务 UI 更新必须持显示锁且带超时；UI 构建必须先于 Wi-Fi
  启动（NVS 缓存凭据后 2.4s 即拿到 IP，会早于 UI 构建触发竞态崩溃）。
- **LVGL 图片：必须构建期预解码**（2026-11-15，用户三轮实测）：运行时
  PNG 解码（esp_lv_decoder）缓存 256KB 装不下 9 张 88×88 解码结果
  （≈279KB）→ 反复逐出重解码；且 ARGB+缩放（scale≠256）在滑动翻页/
  转场的全屏重绘中堵死显示管线（表现为"滑不动/卡死"）。现行方案：
  `scripts/gen_icon_embed.sh` 构建期转 **RGB565 108×108 原生数组**
  （零解码/零缩放/零混合），真机 1:1 显示。素材含透明通道才需 ARGB8888。
- **滚动回调里禁止改布局**（2026-11-15）：LV_EVENT_SCROLL 逐像素触发，
  在回调里改页点尺寸（lv_obj_set_size）会逐帧触发全屏重排+重绘 → 与
  显示管线叠加卡死。页点只在页号变化时更新。
- **桌面磁贴打开判定用按下/抬起位移**（≤10px 且 ≤600ms，且需
  `lv_indev_get_press_moved()==false`）：LV_EVENT_CLICKED 在 tileview
  横滑后仍会误发（滑动时误开 App，用户实测三轮回馈）。
- **翻页瞬切，不做全屏滑动动画**（2026-11-15，用户实测花屏）：每次
  翻页动画要连刷 480×360（≈345KB/帧 ×12 帧），连续滑动会灌爆 QSPI
  DMA 队列 → 花屏+卡死（同坑#2 机理）。桌面翻页改为
  `set_tile_by_index(..., LV_ANIM_OFF)` 单帧切换。
- **性能配置必须对齐官方 adapter README**（2026-11-15，重大发现）：
  本项目固件此前是 `-Og + 160MHz + 单绘制单元 + 33ms 刷新 + LV_OS_NONE`，
  而官方 `examples/esp-idf/02_lvgl_demo_v9` 与 esp_lvgl_adapter README
  推荐 `-O2 + 240MHz + DRAW_UNIT_CNT=2 + LV_OS_FREERTOS + 15ms +
  CLIB malloc/string/sprintf + SPIRAM_XIP_FROM_PSRAM + 32KB I-cache +
  64KB D-cache + FREERTOS_HZ=1000`——渲染速度差 3~5 倍，是桌面滑动
  卡死/官方固件更流畅的根本原因。已对齐 **-O2 / 240MHz / 15ms 刷新 /
  CLIB / FREERTOS_HZ=1000**（这四项是 3~5x 提速的主要来源）。
  **未采用**：`SPIRAM_XIP_FROM_PSRAM`+大 cache（撤：PSRAM 取指与显示
  DMA 争用，撤后 internal 84→132KB）、`DRAW_UNIT_CNT=2`（撤：见下方
  "线程绘制 + 任务通知" 坑，改 1 换确定性）。internal RAM 因此收紧，
  显示缓冲仍须在启动期分配。
- **禁止手指直接拖动 tileview**（2026-11-15，压测定位）：手指连续拖拽
  时 tileview 每帧全屏重绘（背景 canvas 图 + 4 图标合成），占空比 100%
  → 显示管线压垮卡死；同样的 60 次程序化翻页（每 800ms，动画 200ms）
  内存水位完全稳定（PSRAM 恒定 7,857,112B）且零卡顿——**不是内存泄漏**。
  现行：`LV_OBJ_FLAG_SCROLLABLE` 关闭 + 自研快滑手势（30ms 轮询指针，
  dx≥50px、2:1 主导、≤500ms）→ `set_tile_by_index(ANIM_ON)` 整页切换，
  重绘成本有界（每滑一次 ≈200ms 动画）。
- **`bsp_display_lock(0)` 不是无限等待**（2026-11-15，开机白/绿半屏死机
  定位）：BSP 头文件写"0 will block indefinitely"，实现却是转
  `esp_lv_adapter_lock(0)` = **0 tick 的 try-lock**，失败只返回 false。
  返回值一被忽略，main 就会在 adapter 渲染中并发改 widget → 命中
  `LV_ASSERT_MSG(!disp->rendering_in_progress, "Invalidate area is not
  allowed during rendering")`；而 `LV_ASSERT_HANDLER` 默认是
  `while(1);`（`CONFIG_LV_USE_LOG` 关闭时**一个字都不打印**）→ 主任务
  永久空转、IDLE0 每 30s 报 task_wdt、屏上只剩半帧（上白下绿）。
  串口现场：`task_wdt: CPU 0: main` + backtrace
  `lv_inv_area(lv_refr.c:282) ← lv_obj_invalidate_area ← lv_obj_set_pos
  ← lv_screen_load_anim ← lv_screen_load ← ui_init(main.c:561)`。
  修法：一律 `esp_lv_adapter_lock(-1)`（`ui_lock/ui_unlock` 与 main 的
  `display_lock()` 包装器），并开 `CONFIG_LV_USE_LOG`（ERROR 级）让
  LVGL 断言先打印再挂。
- **背光必须延后到首帧构建完**（2026-11-15）：`ui_init()` 之前调
  `bsp_display_backlight_on()` → `esp_lcd_panel_io_tx_param(0x51)`
  会和 adapter 首帧刷屏的 DMA 抢同一路 QSPI/SPI 总线，main 永久阻塞在
  该调用（实测：main=Blocked、lvgl=Blocked、**显示锁空闲**、无 WDT）。
  修法：`ui_init()` + `saver_build()` 之后再开背光（顺带避免开机闪一屏
  未初始化的花屏）。真机复测：post-ui 3824ms → 背光 3825ms → Wi-Fi
  5154ms，heap_int 稳定 ~80KB，lvgl 心跳正常。
- **伴宠帧动画走独立 assets 分区**（2026-11-15）：帧序列（5 个动作集
  ×10~12 帧 RGB565A8 ≈2.23MB）app 分区装不下（只剩 ~240KB），OTA 分区
  又不能挪作数据——分区表新增 `assets, data, undefined, 0xC10000, 2560K`
  （ota_1 5M→4M 腾位）。工具链：`scripts/gen_companion_bin.sh`（PNG →
  CMP1 blob：header + 32B 条目表 + RGB565A8 帧平面，LVGL 原生格式零解码）
  + `scripts/flash_companion.sh`（烧 @0xC10000；改分区表要同步改脚本偏移）。
  运行时 esp_partition_read 到 PSRAM 帧缓冲逐帧换图，原生尺寸零缩放
  （避免 SW transform）。分区表重刷不影响 factory/NVS（偏移未动）。
- **快速音量**（2026-11-15）：点状态栏右侧图标簇弹滑条（3s 自动关），
  ds_config_update + audio_esp32_set_volume（codec 0~100 直映）；开机由
  状态栏首次刷新把 Web 配置音量套到 codec（audio init 早于数据源）。
- **线程绘制 + 任务通知同步会和 adapter 抢同一个通知**（2026-11-15）：
  `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2` + `CONFIG_LV_USE_FREERTOS_TASK_NOTIFY=y`
  时，draw unit 的条件变量用 FreeRTOS task notification，而
  esp_lv_adapter 的 LVGL 任务自己也用 `ulTaskNotifyTake` 做循环等待
  → 通知被吞/丢失唤醒，首帧渲染可能死等（lvgl 持锁 Blocked、探针试锁
  BUSY）。现行先取确定性：`LV_DRAW_SW_DRAW_UNIT_CNT=1` +
  `# CONFIG_LV_USE_FREERTOS_TASK_NOTIFY is not set`；要恢复双绘制单元
  需先单独压测这条路径。

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

- [x] i2cdetect 扫描全部 I2C 地址，确认 2.6 节 ⚠️ 项。（2026-11-14 真机全 ACK，见 §2.7）
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

### 2.9 Muse 官方固件实测与 NVS 布局（2026-11-15）

上游 `muse-gadget-sdk` 已内置本板（`devices/waveshare-s3-216.md`），
`tools/muse/board.sh build s3-216` 在 IDF v6.0.3 下一次通过，2.17MB。

实机验证（USB 供电、无电池）：

- 启动正常：`link.identity: node_id=homelink-79bac4 ble_name=MuseGadget-79BAC4`
- 音频自检：`capture 16132 Hz, playback 16161 Hz`（ES8311/ES7210 均 OK）
- Wi-Fi 扫描 7 AP、BLE 广播待配对；配对后 `setup=done ws=up`（Noise 会话连通）
- 内部 RAM：启动后 `int=33K/18K`（扫描时 `min=7K`）——远紧于本固件（空闲 72K）

**Muse 固件分区表**（与我们的不同，刷回本固件后此表失效）：

| 分区 | 偏移 | 大小 |
|------|------|------|
| nvs | 0x11000 | 0xC000（48KB） |
| otadata | 0x1D000 | 0x2000 |
| ota_0 | 0x20000 | 4M |
| ota_1 | 0x420000 | 4M |
| apps/files (LittleFS) | 0x830000 / 0xA30000 | 2M / 4M |

**凭据导出**（配对后，Bridge 用）：`esptool read-flash 0x11000 0xC000`，
再用 IDF 的 `nvs_tool.py -d minimal` 取 `homehub:` 命名空间下的
`access_token` / `refresh_token` / `node_id` / `noise_host` / `api_url_v2`。

注意：Muse 的 `files` LittleFS（0xA30000–0xE30000）与我们的 `assets`
分区（0xC10000–0xF90000）重叠——刷回本固件后必须重跑
`scripts/flash_companion.sh`，否则伴宠资源被清。

### 2.10 启动期显示管线挂死（wedge，未解，2026-11-15 归因实验）

**症状**：复位启动后 ~8–14s，LVGL 任务持锁卡死（`probe#N lvgl_lock=BUSY`
不再恢复），随后控制台无输出（其余任务看似存活，esptool 仍可通信复位）。
复位彩票：同一固件连续复位，约 30–50% 概率触发；好启动时一切正常。

**归因实验（2026-11-15）**：
- 优先级继承证明持锁者是 **lvgl 任务本身**（cfg_fetch 若持锁会被 lvgl
  任务的优先级 6 抬升，实测卡死时 cfg_fetch 仍为创建优先级 4）→ 卡在
  lv_timer_handler / flush 内部，即显示管线类问题（同 §2.7 坑#2 家族）；
- 对照组：**改动前固件（HEAD 无运行时重建桌面）8 轮复位 wedge 4 轮
  （50%）**，新固件约 30% → **先前已存在**，与"联网后重拉主页排布并
  重建桌面"的改动无关（该改动在好启动上验证通过：layout applied +
  remote config applied + 探针持续 FREE）。

**排除项**：power_manager_snapshot（缓存读，I2C 在独立任务）、状态栏
定时器（只读 ds_state 缓存）、屏保（2 分钟才触发）。

**2026-11-15 深挖（自愈已落地）**：
- 楔死瞬间任务表：`lvgl` 任务状态 **B（Blocked）**、两个核都跑 IDLE →
  不是死循环，是 lvgl 任务阻塞在某个等待（flush/DMA 回调类）；
- hangprobe 里带超时的 `esp_lv_adapter_lock` 会**把自己卡死**（日志停在
  probe#2），改成 `lock(0)` try-lock 后同样卡在锁调用里——所以软件探针
  无法在楔死时继续工作；
- **对照实验**：完全移除 hangprobe 后复位 8 轮，仍 4 轮楔死 → 探针不是
  诱因，缺陷与探针无关（概率仍 ~50%）；
- **自愈方案（已实现）**：hangprobe 注册任务看门狗（每 5s 喂），楔死时
  TWDT 15s（`CONFIG_ESP_TASK_WDT_TIMEOUT_S=15` + `PANIC`）→ panic 重启。
  实测：8.4s 楔死 → 28.5s 自动重启（`task_wdt: Aborting` + `rst:0xc`），
  无需人工断电；坏启动后大概率下一次正常。
- 仍待 root-cause：阻塞点的堆栈（需在楔死时用 TWDT panic 的 backtrace
  或 JTAG 抓 lvgl 任务栈）。
