# 出厂系统架构审查：扩展原厂 vs 基于官方框架重建

> 2026-11-14 设备到手实测后方向调整（用户拍板）：不再推倒重做出厂系统，
> 保留原厂体验并扩展。本文是对微雪官方仓库的架构审查结论，回答
> 「原厂固件扩展」还是「基于官方 BSP 重建并复刻原厂功能」。

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

**决策（ADR-007）**：采用「**基于官方公开框架重建原厂体验 + 应用扩展**」：
esp-brookesia Phone 框架（Launcher/手势/任务列表现成）+ Waveshare BSP
（硬件层现成）+ 我们的自研 apps 经 App::Registry 注册。出厂固件保留设备
恢复用；我们不再维护"从零自研 Launcher"路线的固件仓库主体。

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
