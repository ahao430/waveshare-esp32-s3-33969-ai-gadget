# 决策记录（ADR）

按时间倒序记录影响系统边界的决策。每条包含背景、决策、理由与后果。

---

## ADR-005 三个独立 AI App + 共用 Assistant UI / Companion Engine

日期：2026-11-14　状态：已接受（用户拍板）

**背景**：Muse（官方 SDK）、大肥鱼（OpenAI 兼容 Model Router）、小智
（自有协议/服务端）三者服务接入方式完全不同。塞进一个 App 会导致
聊天引擎里大量协议分支判断；各写一套前端则三倍维护成本。

**决策**：

1. **三个独立业务 App 入口**（Muse / 大肥鱼 / 小智），各自实现服务
   适配器（网络/认证/会话生命周期/协议转换/音频传输/服务端配置）。
2. **共用一套 Assistant UI**：消息气泡、消息列表、语音按钮、状态与
   错误呈现、会话布局交互。文字输入框按形态裁剪（设备端以语音为主）。
3. **共用 Companion Engine（角色动画引擎）**：统一状态接口
   idle / listening / thinking / speaking / success / error——
   与 voice_session 状态机同源；每角色独立资源集
   （Muse=吉祥物 GIF / 大肥鱼=dsh-pet 状态动画 / 小智=Q 版形象）。
   换形象/加动作不动聊天引擎。
4. **音频独占**：三个助手共用 Audio Manager，同一时刻仅一个占用
   采集/播放链路（voice_session 租约，ADR-003 落实）。
5. 大肥鱼 App 内部可动态创建 N 个模型/人设（经 Bridge Model Router），
   不影响另外两个 App。

**代码组织**（固件目录，PLAN §4.2 同步）：

```
apps/assistant/          共用聊天 UI（chat_view/message_list/companion_renderer）
apps/{muse,bigfish,xiaozhi}/  三套服务适配器
components/assistant_core/    公共助手接口（状态事件、消息抽象）
components/companion_engine/  公共动画引擎
```

模拟器侧映射：`ui_conversation.c` = Shared Assistant UI；
`companion.c` = Companion Engine；voice_session = 状态事件源。

**桌面 App 清单（定稿）**：Muse、大肥鱼、小智、音乐播放器、订阅中心、
ZCode Remote、天气（新增）、Komari 服务器监控、文件管理器、系统设置。

**后果**：PLAN §4.2/§5.1/§8.5 已同步；新增天气 App（Phase 5+ 占位）；
后续增加第四个 AI 助手 = 新增一个适配器 + 一套形象资源。

---

## ADR-004 Firmware 基线选择 ESP-IDF v6.0.x

日期：2026-11-14　状态：已接受

**背景**：三个上游对 IDF 版本要求不同：

| 上游 | 要求 | 来源 |
|------|------|------|
| Waveshare 官方示例 / BSP ^2.0.1 | 测试于 v5.5.5 与 v6.0.2 | 官方仓库 README |
| Muse Gadget SDK | 仅 v6.0.3 | esp32/README |
| xiaozhi-esp32 | ≥ v6.0.1，推荐 release/v6.1 | README（5.x 不再支持） |

**决策**：本仓库固件基线选 **ESP-IDF v6.0.x**。实际落地 **v6.0.3**
（2026-11-14 安装时 v6.0.x 系列最新 tag，且恰为 Muse SDK 要求的精确
版本）。Muse 集成按 PLAN 要求独立构建验证，不合入主工程。小智若后续
集成，先在 v6.0.x 下验证其协议/音频层。

**后果**：v6.0.x 是三者交集；Waveshare BSP 在 v6.0.2 官方测试通过，
v6.0.3 同属 6.0 系列（首建已通过）。v6.1（小智推荐）暂不跟进，
等小智集成时单独评估。

---

## ADR-003 三语音引擎独立 App + 共享 Voice Session 服务

日期：2026-11-14　状态：已接受

**背景**：小智、Muse、大肥鱼语音三套语音链路协议、会话归属、UI
形态完全不同（小智协议/小智服务端；Noise/Muse 云端；Bridge
ASR+LLM+TTS/Bridge SQLite）。

**决策**：

1. 每个第三方语音生态一个独立业务 App（`apps/xiaozhi`、`apps/muse`），
   大肥鱼语音走 Bridge ASR/TTS。三者互相不共享会话历史。
2. 公共能力下沉系统服务：`Audio Manager`（I2S/编解码/缓冲，硬件独占）
   + `voice_session`（Push-to-Talk/连续对话、VAD、状态机、独占租约）。
3. 语音引擎只通过 Voice Session 取音频，不直接碰 I2S。
4. v2 可在 Voice Session 抽象上加统一"语音助手壳"（引擎可切换）；
   v1 不做。

**理由**：避免两套会话历史塞进一个聊天 App（不同步、激活流程耦合、
UI 形态冲突）；引擎独占租约符合"一个业务 App 前台运行"的运行模型。

**后果**：PLAN.md §4.2/§5.3/§5.5/§11 已同步更新。

---

## ADR-002 Bridge 首版 HTTP 框架用标准库 net/http，不引入 Gin/Chi

日期：2026-11-13　状态：已接受

**背景**：PLAN §3.3 将 Gin/Chi 列为备选。Phase 0 只需要最小 API
骨架 + Mock Provider。

**决策**：Bridge 首版用 Go 标准库 `net/http`（Go 1.22+ 路由模式匹配
已够用），零第三方依赖起步。待出现真实需求（中间件生态、WebSocket
框架化）再评估引入路由库。

**后果**：`go.mod` 当前无第三方依赖，构建与 CI 最轻。

---

## ADR-001 模拟器 UI 与固件 UI 共享同一份业务 UI 代码

日期：2026-11-13　状态：已接受

**背景**：PLAN §16.4 要求"业务层不能因为运行在 PC Simulator 就引入
另一套业务实现"。

**决策**：Launcher 等 UI 屏幕代码写成纯 LVGL 9 代码（不依赖 IDF 专
用 API），放在 `simulator/lvgl-pc/src/ui/`；固件集成时通过 CMake 把
同一目录编入 ESP-IDF 组件。Mock 数据层（配额/监控/状态）与真实数据
层通过同一接口注入，双端共用。

**后果**：PC 上开发的 UI 可直接进固件；硬件差异只存在于显示驱动层
（SDL vs CO5300 QSPI）和输入层（鼠标模拟 vs CST9217）。
