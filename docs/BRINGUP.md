# Phase 6 真机 Bring-up 手册（33969）

> 目标：设备到手后按本手册顺序执行，每一项有**预期现象、验证命令、
> 失败排查**。发现任何与本文不符的现象 → 记录到文末表格，并回写
> [HARDWARE.md](HARDWARE.md)。
> 配套：硬件事实与引脚表见 [HARDWARE.md](HARDWARE.md)；
> 上游版本结论见 [DEVELOPMENT.md](DEVELOPMENT.md)。

## 0. 开箱与安全

- [x] 外观检查（屏碎/弯针/焊点），拍照留档。
- [x] **先不接电池**：USB 供电即可完成全部显示/触摸/音频验证；
      电池项在 PMU 验证时再接。
- [ ] 静电注意：接触前触摸接地物；AMOLED 面板勿按压。
- [x] 附件确认：USB-C 线（**数据线**，非仅充电线）、备用线一根。

## 1. 首连与烧录（display-first）

### 1.0 备份原厂固件（一次性、不可跳过）

> 出厂固件**闭源**，刷写即覆盖且无法从官方渠道取回。先做全片备份，
> 以后随时可以还原原厂体验（或对照其行为调试）。

```bash
# 识别 16MB flash（esptool flash_id 确认 Detection...16MB）
cd firmware && . $HOME/esp/esp-idf-v6.0.3/export.sh
python -m esptool -p /dev/cu.usbmodemXXX flash_id

# 全片读回（约 1-2 分钟），保存到仓库外稳妥位置
python -m esptool -p /dev/cu.usbmodemXXX read_flash 0 0x1000000 \
    ~/gadget-factory-backup-$(date +%Y%m%d).bin
# 校验大小 = 16777216；再复制一份到移动介质/网盘
```

- 还原（任何时候）：`write_flash 0x0 备份文件`（注意该备份含分区表与 NVS，
  还原后设备与出厂状态一致）。
- 若读回失败 → 换数据线 / 按住 BOOT 插 USB 再试；**不备份不刷写**。
- ✅ **已完成（2026-11-14）**：`~/gadget-factory-backup-20261114.bin`
  （16MB 校验通过，sha256 同目录 `.sha256`；原厂分区表已解析，
  见 HARDWARE.md §2.7——用户记得再复制一份到移动介质/网盘）。

### 1.1 串口识别

```bash
ls /dev/cu.*   # macOS：插 USB 后应出现 cu.usbmodem*（板载 USB-Serial）
```

- 无设备出现 → 换数据线；仍无 → 按住 BOOT(GPIO0) 再插 USB 强制下载模式。

### 1.2 首次烧录（当前仓库骨架固件）

```bash
cd firmware
idf.py -p /dev/cu.usbmodemXXX flash monitor
# 预期：编译产物已备好（scripts/check.sh 已构建）
```

### 1.3 启动日志（第一批证据）

> ✅ **2026-11-14 已完成**：I2C 六地址全 ACK（0x18/0x34/0x40/0x51/0x5A/0x6B，
> 无未知设备）；CO5300 显示 + LVGL + 背光正常；cst9217 识别 CST9220 触摸。
> 详见 HARDWARE.md §2.7。

monitor 里按顺序确认：

1. `=== I2C scan ===` 输出——**这是地址事实的最终裁定**。
   预期 ACK 的地址及对应芯片：

   | 地址 | 预期芯片 | 若缺失 |
   |------|----------|--------|
   | 0x34 | AXP2101 PMU | 电源管理不可用，先继续其他项 |
   | 0x51 | PCF85063 RTC | 待查（社区默认值待验证） |
   | 0x6B | QMI8658 IMU | 待查 |
   | 0x18 | ES8311 codec | 试 0x19（AD 引脚变体） |
   | 0x0x40 | ES7210 ADC | 试 0x41/0x42/0x43（AD 引脚变体） |
   | 0x5A | CST9220 touch | 试 0x5B |

2. `bsp_display_start` 无 crash；屏幕显示
   "AI Gadget bringup build - Phase 6 prep"。
3. 对照记录：扫描到的真实地址 → **回写 HARDWARE.md §2.6**。

## 2. 逐驱动验证清单（按依赖顺序）

每项 = 预期现象 + 怎么验 + 失败排查。做完一项勾一项，并记录偏差。

### 2.1 显示（BSP，骨架已带）— ✅ 2026-11-14 基本通过

> 实测：亮度滑条 10-100% 实时生效；触摸指示器+坐标正常。
> **运行时旋转不可用**：`bsp_display_rotation_set` 只改面板 MADCTL，与 LVGL
> 帧缓冲不同步（屏上出现多朝向残影）；LVGL 级旋转（esp_lv_adapter）仅在
> init 配置时可设。产品用固定方向（圆屏），不做运行时旋转。

- [x] 屏幕点亮、显示文字清晰（骨架 build 文案）。（2026-11-14 用户确认）
- [x] 亮度：调用 `bsp_display_brightness_set(50%)` 目测变化
      （AMOLED 走面板命令 0x51，无独立背光引脚——见 HARDWARE.md §2.2）。
      （滑条 10-100% 实时生效）
- [x] 旋转：`bsp_display_rotation_set` 四方向目测。（MADCTL 通道工作，
      但与 LVGL 帧缓冲不同步→残影；运行时旋转弃用，见上方说明）
- [ ] **PSRAM 缓冲压力测试**（Muse 坑 #1）：Wi-Fi 启用后连续刷新
      30s 无花屏/无 crash。失败 → 显示缓冲迁内部 RAM 分行绘制
      （参考 Muse `muse_lcd_bands.c` 思路）。

### 2.2 触摸 — ✅ 2026-11-14 通过

- [x] LVGL 输入：触摸滑动光标（monitor 观察坐标日志或加临时日志）。
      （紫色指示器+坐标跟随手指，用户目视确认；滑条/按钮交互全正常）
- [x] 单点确认（Muse 坑 #4：仅单点、LVGL 手势默认关）。（单点足够驱动全部 UI）
- [ ] 触摸唤醒（§2.9 一并验证）。

### 2.3 音频（ES8311 播放 / ES7210 录音）— ✅ 2026-11-14 通过

- [x] 播放：BSP audio init + codec_speaker_init，播 440Hz 正弦 2s，
      **接耳机/喇叭听得到**（PA GPIO46 已在 BSP 使能）。
      （用户确认听到；write 阻塞 2024ms = DMA 完整消费）
- [x] 录音：codec_mic_init + I2S 读，检查采样非全零（静音板载麦在
      无声环境下读数应仍有底噪波动）。（peak 24576+，说话有效；
      注：42dB 增益会削波已降 24dB，RMS 溢出已修 int64）
- [ ] 失败排查：i2c 扫描确认 codec 地址存在；I2S 引脚对照 HARDWARE.md
      §2.4；PA 使能电平用万用表测 GPIO46。（未触发，无需排查）

### 2.4 TF 卡 — ✅ 2026-11-14 通过（1-bit 模式）

- [x] 插卡 → `bsp_sdcard_mount` 成功，`ls /sdcard` 可见。
      （cid.name=ASTC，BSP 默认 1-bit 模式挂载成功）
- [x] 写入 1MB 文件 + 回读校验（吞吐粗测：`time dd`）。（32B 写读校验 OK；
      1MB 吞吐粗测待音乐 App 阶段再做）
- [ ] 未插卡启动：系统正常起（PLAN §12.2 要求降级路径）。（待拔卡验证）

### 2.5 AXP2101 PMU — ✅ 2026-11-14 部分（USB 供电态）

- [x] 0x34 ACK；读芯片 ID 寄存器（0x03）= 0x4A？确认。（**reg03=0x4A 属实**；
      另 reg00=0x38 reg01=0x32，对表解释待电池接入）
- [ ] 电池电压/充电状态寄存器读取。
- [ ] 接电池后：电压与万用表实测一致。
- [ ] **PWR 键行为**（Muse 坑 #2/#3）：IRQ 未接 ESP32 → PWR 不能从
      light sleep 唤醒；运行中长按 PWR 不关机（固件关了 PMU 关机保持）。
      复核这两条，写入性能记录。

### 2.6 RTC（PCF85063）— ✅ 2026-11-14 读通

- [x] 0x51 ACK；写/读秒寄存器走通。（BCD 时分秒读出；时间为随机值
      ——从未校时，符合预期。写校时待系统时钟接入时验证）
- [ ] 断电（仅 USB 断）后纽扣电池维持时间（若板带电池座）——查原理图。

### 2.7 IMU（QMI8658）

- [ ] 0x6B ACK；读 WHO_AM_I（0x00）= 0x05。
- [ ] 姿态数据随板子转动变化。

### 2.8 按键 — KEY3 已接线待真机确认

- [ ] BOOT(GPIO0)：下载模式进入。（刷机用，暂未单测）
- [ ] KEY3(GPIO18)：上拉输入，按下读 0。
      它是**唯一的可用唤醒键**（Muse 坑 #2）。（ISR 已接 TONE，待按键实测）

### 2.9 电源/睡眠

- [ ] 息屏（display sleep 0x10/0x11 命令，PLAN §13）。
- [ ] light sleep 进入/退出；KEY3 唤醒。
- [ ] 电流实测（见 §3 记录模板）。

### 2.10 Wi-Fi

- [ ] 连接家庭 AP；RSSI 合理。
- [ ] HTTP GET Bridge `/api/v1/device/status` 通。
- [ ] 连续 30 分钟无断连重连风暴。

## 3. 性能与功耗记录模板（PLAN §14.3）

每完成一轮，把数值填进下表并提交到本文件"实测记录"节。

```text
日期/固件版本：
SRAM 剩余（启动后/负载后）：
最大连续空闲块：
PSRAM 剩余：
CPU 占用（LVGL 刷新时/空闲时）：
固件大小：
App 切换内存变化：
音频播放峰值内存：
Wi-Fi 断开恢复时间：
电流：亮屏待机 / 息屏 / Wi-Fi 活跃 / 音频播放 / light sleep
电池续航（实测场景 + 时长）：
```

## 4. 已知板级坑 checklist（Muse 移植经验，逐条复核）

| # | 坑 | 复核方法 | 结果 |
|---|----|----------|------|
| 1 | PSRAM 显示缓冲在 Wi-Fi/BLE 后失效 | §2.1 压力测试 | ☐ |
| 2 | AXP2101 IRQ 未接 → PWR 键不能唤醒 light sleep | §2.5/§2.9 | ☐ |
| 3 | 运行中长按 PWR 不关机 | §2.5 | ☐ |
| 4 | 触摸仅单点、手势库默认关 | §2.2 | ☐ |
| 5 | IMU/RTC/TF 无上游参考实现 | §2.4/2.6/2.7 首验 | ☐ |
| 6 | 内部 RAM 紧张（Wi-Fi 后曾低至 6KB） | 启动+Wi-Fi+音频并发下 heap 监控 | ☐ |

## 5. 偏差记录表（发现问题填这里，回写 HARDWARE.md）

| 日期 | 现象 | 证据（日志/测量） | 处置 |
|------|------|--------------------|------|
|      |      |                    |      |
