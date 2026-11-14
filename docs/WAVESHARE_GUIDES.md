# 微雪官方教程索引与要点（33969）

> 官方文档入口：https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/
> 本文收录三篇教程的关键信息与本仓库流程的对照（2026-11-14 抓取整理）。
> 官方仓库审查见 [FACTORY_REVIEW.md](FACTORY_REVIEW.md)；真机 bring-up
> 执行手册见 [BRINGUP.md](BRINGUP.md)。

## 1. 教程索引

| 教程 | URL | 用途 |
|------|-----|------|
| 烧录/使用说明 | docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/Instructions-For-Use/ | 出厂固件烧录/擦除/恢复 |
| ESP-IDF 开发 | docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/ESP-IDF/ | 五个官方示例的构建烧录流程 |
| Arduino 开发 | docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/Arduino/ | 七个 Arduino sketch + 库清单 |
| 小智 AI | docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/XiaoZhi_AI/ | 刷 xiaozhi-esp32 整包固件（覆盖出厂，见 FACTORY_REVIEW §8） |

## 2. 烧录/擦除要点（出厂恢复）

- 工具：乐鑫 Flash Download Tool；ESP32-S3 + USB；波特率 1152000；
  **烧录地址 0x00**；bin 在示例包 `firmware/` 目录
- "等待上电同步中"卡住：按住 BOOT 重新上电进下载模式
- **整片擦除**：不选任何 bin → START → ERASE（恢复出厂前建议先擦除，
  清掉 NVS 里的配网/小智激活残留）
- 进 Boot 模式：按住 BOOT → 接 USB → 松开

**与本仓库对照**：我们用 `idf.py flash`（源码构建）与 esptool；恢复
出厂用官方 factory bin @0x0（BRINGUP §恢复出厂）。两者不冲突：日常
开发刷自研固件，需要"回到原厂"时刷 factory bin。

## 3. 出厂固件 App 清单（官方文档首次确认）

固件基于 brookesia 组件（与 03_esp-brookesia 示例同框架），预装：

| App | 说明 |
|-----|------|
| DrawPanel | 触摸绘图板 |
| squareline | LVGL demo |
| Calculator | 计算器 |
| **AIChats** | **小智 AI（与小智官网版本一致，双击 BOOT 返回主界面）** |
| SpecAnalyzer | 实时音频频谱 |
| Gravity Ball | 六轴重力小球 |
| VideoPlayer | AVI 播放（SD 卡 avi/ 目录） |
| MusicPlayer | **"音乐播放器界面模拟"**（官方文档原话） |
| Settings | WiFi 扫描与**扫码配网**、亮度/音量、电池电压显示 |

三个关键确认：

1. **AIChats（小智）在出厂系统就是 App 形态**（双击 BOOT 返回主界面 =
   app 与桌面共存）——印证我们自研固件"小智作为 App 集成"的路线
   （xiao-zhi 协议客户端 App，XIAOZHI_PROTOCOL.md）。
2. **MusicPlayer 官方自述"界面模拟"**——用户观察到的"后台播放"需真机
   复核（可能是 UI 模拟而非真实后台解码）；POWER_MANAGEMENT 后台策略
   的验收基准以真机实测为准。
3. **Settings 就是扫码配网**（与我们 Web 二维码主路径一致）+ 小屏
   WiFi 扫描连接并存——ADR-008 的配网双路径与原厂完全同构。

## 4. Arduino 路线（对照参考）

- 核心版本 Arduino-ESP32 **3.3.11**（06_ES7210 依赖其 esp_vad.h）
- 库：GFX Library 1.6.4（Arduino_CO5300 驱动）/ SensorLib 0.3.3
  （CST9220+QMI8658）/ XPowersLib 0.2.6（AXP2101）/ lvgl 8.4 + 配套
  lv_conf.h / Mylibrary（引脚宏）
- 示例：01_HelloWorld → 02_GFX_AsciiTable → 03_LVGL_AXP2101_ADC_Data
  （**PMU 电源数据显示**）→ 04_QMI8658 折线 → 05_LVGL_Widgets（音乐
  demo + IMU 自动旋转）→ 06_ES7210（**麦克风 + VAD 人声检测**）→
  07_ES8311（播放 + 回环）

**对本项目的价值**：我们不采用 Arduino 路线（ESP-IDF 组件体系已建成
且 LVGL v9），但 Arduino 示例是**逐硬件的独立验证器**——真机 bring-up
（BRINGUP Phase 6）可借它们的顺序：AXP2101 电源读数 → 触摸 → 音频
采集/回环，逐项验证后再跑自研固件。XPowersLib 的 AXP2101 读取逻辑
可直接移植进 power_manager（替换 bring-up 时的寄存器 TODO）。

## 5. ESP-IDF 路线（我们的主线）

五示例（01_AXP2101 / 02_lvgl_demo_v9 / 03_esp-brookesia /
04_Immersive_block / 05_Spec_Analyzer），测试线 IDF v5.5.5 + v6.0.2
（我们 v6.0.3 构建验证通过，见 FACTORY_REVIEW）。构建流程与我们
scripts/check.sh 的 firmware 段一致。03_esp-brookesia 的交互范式
（四宫格/上滑退出/任务列表）是自研 Launcher 的学习对象（ADR-008）。
