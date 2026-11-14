# 出厂系统架构审查：扩展原厂 vs 基于官方框架重建

> 2026-11-14 设备到手实测后方向调整。**最终路线见 ADR-008：固件基线
> 回到自研**——本文定位为交互范式与社区项目学习资料（esp-brookesia
> 操作方式/架构设计 + 社区项目调研），不作框架基座。

## 1. 结论先行

**出厂系统源码未公开，无法直接在其上扩展**；但出厂体验的技术底座全部
公开可得，"重建出厂体验 + 扩展应用"的成本远低于预期——

| 你的问题 | 审查结论 |
|---------|---------|
| 出厂固件是否有公开源码 | ❌ 无。官方仓库 `firmware/` 是不可变的恢复镜像，"source and build instructions for the factory image are not included in this repository" |
| Launcher 支持注册新应用吗 | ✅ **有公开的 App 注册表机制**。esp-brookesia `App::Registry` 插件注册表：应用经注册宏自注册，`Phone::initAppFromRegistry/installAppFromRegistry` 遍历安装 |
| 应用切换/任务管理如何实现 | esp-brookesia **Phone 系统**原生提供：四宫格 App Launcher、左右分页、上滑退出、RecentsScreen 任务列表（main.cpp 里 `getRecentsScreen()->setMemoryLabel` 即任务页内存面板）、状态栏时钟 API |
| 音乐后台是独立任务还是媒体服务 | ❓ 出厂实现未公开。但 App 基类有 `pause()/resume()` 生命周期钩子（esp_brookesia_base_app.hpp:293/314）——**"按应用能力管理后台"的挂点框架已备好**，音乐后台需自实现（按 AIVault/QuotaFrame 参考架桥） |
| LVGL 是否统一 Display Manager | ✅ 是。esp-brookesia 统一管理（Phone/Display/StatusBar/NavBar），BSP 提供 `bsp_display_lock/unlock` 与 LvLock 集成 |
| 能否不改原厂应用添加新应用 | ✅ 原厂应用=出厂固件闭源；**但基于 esp-brookesia 重建的系统里，新应用=实现 `systems::phone::App` 子类 + Registry 注册，完全不触碰框架与其他应用** |

**决策演进**：ADR-007 曾选"框架重建"；ADR-008（最终）改为**固件基线
回到自研**，esp-brookesia 仅作交互范式学习对象——不引入框架依赖，
自研 Launcher 按 esp-brookesia 的交互范式实现（四宫格/左右分页/上滑
退出/任务列表/pause-resume 生命周期语义）。

> **构建验证（2026-11-14）**：03_esp-brookesia 在本项目 IDF v6.0.3
> 工具链下完整编译通过（1937 个编译单元，产出 esp-brookesia.bin），
> 组件链（brookesia_core 0.6.0-beta2 + BSP 2.0.1 + lvgl 9.x）与
> ADR-004 版本线兼容——重建路径技术可行性已实证。

## 2. 官方仓库事实（github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16）

- `examples/esp-idf/` 五个官方示例（01_AXP2101 / 02_lvgl_demo_v9 /
  03_esp-brookesia / 04_Immersive_block / 05_Spec_Analyzer），测试线
  **ESP-IDF v5.5.5 与 v6.0.2**（与我们 ADR-004 的 v6.0.x 一致）
- `firmware/`：出厂恢复镜像（FactoryOnly-260318.bin），**无源码**
- `examples/esp-idf/03_esp-brookesia/`：手机式 UI 完整示例（111 行
  main.cpp）——**与出厂系统同框架**：状态栏/导航栏/App Launcher/
  手势交互/任务列表
- BSP 组件 `waveshare/esp32_s3_touch_amoled_2_16 ^2.0.1` 依赖链：仅
  esp_codec_dev / esp_lcd_co5300 / esp_lvgl_adapter / lvgl / usb /
  cst9217——**不含 XiaoZhi**（xiaozhi-esp32 issue #1947 描述有误）。
  出厂固件的 XiaoZhi/音乐/相册等集成源码均未公开

## 3. esp-brookesia 扩展机制（我们新应用的接入点）

```cpp
// 1) 应用 = systems::phone::App 子类（参考 brookesia_app_squareline_demo 模板）
class BigfishChatApp : public systems::phone::App {
    bool run() override;    // 进前台：构建 UI（现有模拟器 ui_conversation 移植点）
    bool back() override;   // 返回请求
    bool pause() override;  // 离前台：按后台策略停任务/保留连接
    bool resume() override; // 回前台：恢复
};
// 2) Registry 注册后 Phone 启动时自动装进 Launcher 四宫格
```

- 生命周期 = pause/resume 钩子 → POWER_MANAGEMENT.md「按应用能力管理
  后台」的策略表逐应用落到这两个函数
- **ai_framework 现成 AI Agent 聊天应用模板**（`coze_chat_app`：init/
  start/stop/pause/resume/wakeup 完整生命周期）——大肥鱼/AI 助手类
  应用有官方参考实现
- 应用 UI 可用 Squareline Studio 导出（官方模板即此流程），也可纯
  LVGL 手写

## 6. 社区项目调研：QuotaFrame（eMUQI/QuotaFrame）

**架构**：CodexBar CLI（桌面取数）→ Python Bridge → 加密 BLE（NUS，
JSONL）→ ESP32 固件 → 屏幕显示；协议 `usage.v1` 只发用量百分比/
重置时间/采样时间，**不传凭据**。与我们同款目标板（Waveshare AMOLED
2.16 为首要 target）。

**对订阅展示的可借鉴点**：

1. **数据三态语义**：`ok / partial / unavailable`——"Bridge 取数失败
   时继续发布最后有效数据，保留原始状态与 sampled_at；仅蓝牙断开或
   从未取得有效数据时显示灰态"。对齐我们 Bridge 的 snapshot_cache
   stale 语义，订阅页可升级为三态显示（有效数据常驻 + 灰态仅从未
   有数据时）。
2. **重置倒计时**：设备端卡片右上角实时倒计时（reset_at →
   "xx:xx:xx 后重置"），比静态时间戳更直观——本仓库订阅页已落地。
3. **字段范围最小化**：Bridge→设备只传 percent/reset/sampled_at，
   凭据不出 Bridge——与 PLAN §19 安全边界一致。
4. **组件化固件**：usage_core/usage_ble/usage_protocol/usage_panel_state
   分组件——我们的 companion/input 服务按同样粒度拆分。
5. **BLE（可选增量）**：NUS 传输 + ACK/seq/能力协商/boot_valid 启动
   自检 + BLE OTA。我们 Bridge→设备走 WiFi HTTP（已实现，更简单），
   BLE 桥留作无 Wi-Fi 场景的后续扩展。

其余社区项目：Winamp 播放器（音乐 app UI/音频参考）、VibePulse/
Clawdmeter/Codex Usage Display（通知与状态显示范式）——按需在对应
应用开发时再调研。

## 8. 澄清：出厂系统里的"小智" vs 官方 XiaoZhi 教程（2026-11-14 补）

官方 [XiaoZhi_AI 教程](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/XiaoZhi_AI/)
烧的是 **xiaozhi-esp32 独立固件（整包覆盖）**——FAQ 原话："示例程序与
小智固件是两个独立的程序，无法同时使用或一键切换，需要重新烧录固件
才能切换"。刷入后无 Launcher/桌面，纯语音设备；恢复原厂需重刷出厂
恢复镜像。

因此设备上的"小智"有两个互斥形态：

| 形态 | 来源 | 桌面 |
|------|------|------|
| 出厂系统内置（用户实测） | 出厂闭源固件的微雪自集成（源码未公开） | 保留，小智为四宫格入口 |
| XiaoZhi_AI 教程刷入 | xiaozhi-esp32 开源固件（78/xiaozhi-esp32，整包替换） | 无 |

**对我们自研固件的接入路径**：xiaozhi-esp32 官方仓库开源（协议/唤醒/
音频全开源）——把其 WS 协议客户端作为自研系统里的一个 App/服务集成
（非刷整个固件），实现"小智与桌面共存"，即 PLAN 小智 App 设计
（XIAOZHI_PROTOCOL.md）。出厂细节可对齐：PWR 长按 6s 关机（PMU 行为）、
BOOT 键唤醒（纳入按键映射，POWER_MANAGEMENT §13.4）、小智配网 =
设备热点 192.168.4.1（与我们保留的小屏配网范式一致）。

## 7. 配网策略（ADR-008 §3）

- **主路径**：Web 二维码配置（后台页全屏大码，已实现于设置→网络与
  连接）——小屏输入成本高，桌面/手机浏览器操作效率高。
- **保留**：小屏 Wi-Fi 直连入口（原厂方式，适合无电脑场景）——
  设置→网络与连接加"屏幕配网"项：设备起临时热点 + 软键盘/表单
  （Phase 6 真机实现）。

## 4. 对现有仓库资产的影响

| 资产 | 处置 |
|------|------|
| 自研固件仓库主体（firmware/） | 转为 esp-brookesia 框架底座重建（ADR-007），app 代码从模拟器 UI 移植 |
| 模拟器 UI（apps/assistant 等 C 代码） | 应用业务逻辑可移植；UI 骨架换 esp-brookesia App 基类 |
| Bridge / Web / 订阅 / AstrBot 对接 | **全部保留不变**（协议层与设备 UI 无关） |
| 出厂固件 | 设备保留可刷（恢复出厂）；不再作为我们的开发基线 |
| POWER_MANAGEMENT.md 后台策略表 | 逐应用落到 esp-brookesia pause/resume 钩子 |

## 5. 四阶段路线（用户拍板）→ 落到仓库

1. **摸清原厂固件**：本文即交付（源码不可得的结论 + 框架机制审查）。
   增补：真机上玩透出厂系统并记录交互细节（音乐后台行为、任务列表
   表现、息屏策略），作为重建体验的验收基准。
2. **验证独立应用集成**：以 03_esp-brookesia 为底座，写一个最小
   `systems::phone::App`（订阅额度展示——数据链路现成）注册进
   Launcher 真机验证。
3. **迁移社区功能**：QuotaFrame 的 Bridge→BLE→设备数据桥（额度缓存
   语义已由我们 Bridge snapshot_cache 实现对齐）；Winamp 播放器 UI/
   音频实现作音乐 app 参考。
4. **宠物收藏馆**：框架稳定后投入；Companion 素材管线与 8 动作规范
   （COMPANION_ASSETS.md）直接沿用。
