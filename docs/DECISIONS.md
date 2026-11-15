# 决策记录（ADR）

按时间倒序记录影响系统边界的决策。每条包含背景、决策、理由与后果。

---

## ADR-006 会话历史统一存 Bridge，设备端只存最近列表

日期：2026-11-14　状态：已接受（用户拍板）

**背景**：大肥鱼多角色多会话（PRODUCT_SPEC §1/§3）需要长期历史存储；
设备只有 16MB Flash + TF 卡。

**决策**：大肥鱼与 Muse 的历史会话**统一存储在 Gadget Bridge**
（SQLite：conversations/messages/profiles 表）；设备端只保存最近
会话列表和必要缓存（最近会话的消息上下文）。小智官方云端历史仍以
官方服务为准，设备只维护当前通信状态与最近交互缓存。

**理由**：
- 16MB Flash 不承担聊天记录增长；TF 卡保留给媒体/日志。
- 手机/网页端后续可复用同一份 Bridge 历史（多端查看）。
- Bridge 已有 SQLite/凭据/认证基础设施，会话表增量成本小。

**后果**：
- Bridge 新增 conversations 域（API 草案见 ARCHITECTURE §4a）：
  `GET/POST /api/v1/chat/conversations`、`/conversations/{id}/messages`、
  profiles CRUD。
- 设备聊天页 = 最近会话列表 + 当前会话上下文缓存（NVRAM 预算内）。
- 上下文压缩（摘要 + 最近 N 轮）由 **Bridge 侧**执行（模型窗口管理
  在 Bridge 的 Model Router 处最自然），设备无感。

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

------------------------------------------------------------------------

## ADR-007　设备固件转向「esp-brookesia 框架重建原厂体验 + 应用扩展」

日期：2026-11-14　状态：已接受（设备到手实测后用户拍板）

**背景**：出厂系统实测体验完整（四宫格 Launcher/左右分页/上滑退出/
任务列表/音乐后台播放/小智集成）。官方仓库审查（见
[FACTORY_REVIEW.md](FACTORY_REVIEW.md)）确认：出厂固件源码未公开；
但其框架底座 esp-brookesia（Phone 系统 + App::Registry 应用注册表 +
run/back/pause/resume 生命周期）与 Waveshare BSP 全部公开，且官方
示例 03_esp-brookesia 与出厂系统同框架。

**决策**：不再维护"从零自研 Launcher/任务管理"的自研固件路线为
主基线；固件主体转向 **esp-brookesia Phone 框架 + Waveshare BSP +
自研 apps（App::Registry 注册）**，复刻原厂体验并扩展我们的应用
（宠物收藏馆/大肥鱼/订阅中心/ZCode/天气/服务器监控）。出厂固件保留
设备恢复用。Bridge/Web/订阅/AstrBot 协议层资产全部保留（与设备 UI
解耦）。后台策略表（POWER_MANAGEMENT.md）逐应用落到 esp-brookesia
pause/resume 生命周期钩子。

**后果**：
- Launcher/手势/任务列表/状态栏不再是自研负担（框架现成，对齐出厂
  观感成本低）
- 自研固件已交付的 I2C 扫描/BSP 显示 bring-up 与全仓库检查链保留
- 模拟器 UI 代码向 esp-brookesia App 基类移植是后续主要工程
- 出厂系统的音乐后台/小智集成无法直接复用（源码闭源），音乐后台按
  POWER_MANAGEMENT.md 后台策略自实现

------------------------------------------------------------------------

## ADR-008　固件最终路线：自研固件 + 原厂交互范式学习（修订 ADR-007）

日期：2026-11-14　状态：已接受

**背景**：ADR-007 曾转向"esp-brookesia 框架重建原厂体验"。用户进一步
拍板：esp-brookesia 为闭源出厂系统的开源近亲，**只学习其操作方式与
架构设计**（四宫格/左右分页/上滑退出/任务列表/生命周期钩子范式），
**固件主体回到自研路线**（现有 firmware/ + 模拟器三件套延续）。

**决策**：
1. 固件基线 = 自研固件（不变）。esp-brookesia 与出厂系统仅作交互
   范式与架构参考（App 生命周期 pause/resume 语义、任务列表交互、
   状态栏布局的"学习对象"），不引入框架依赖。
2. **三个顶部按键支持长按/短按**（input 服务统一分发）：
   - PWRON（AXP2101 PMU 键）：**长按 = 开关机（PMU 硬件行为，不改）**；
     短按 = 息屏/唤醒切换（AXP2101 PWRON 短按状态位读数）。
   - KEY3（GPIO18）：短按 = 返回（等价上滑）；长按 = 语音 PTT（唤起
     当前 AI 助手）。
   - BOOT（GPIO0）：短按 = 回 Launcher/首页；长按 = 打开配网页
     （二维码全屏）。
   映射为默认值，后续在设置页可配置。BSP_CAPS_BUTTONS=0：三键均需
   自驱动（GPIO0/18 走 gpio 驱动 + esp-iot-solution button 组件或
   自写去抖；PWRON 走 AXP2101 寄存器轮询，PMU IRQ 未接 ESP32）。
3. **配网策略**：Web 二维码配置为主（后台页全屏大码，已在设置→网络
   与连接实现）；**保留小屏 Wi-Fi 连接入口**（原厂在屏上直连的方式
   适合无电脑场景；键盘输入用设备端软键盘或临时热点+表单）。
4. 社区项目借鉴：QuotaFrame 的 usage.v1 数据语义（ok/partial/
   unavailable 三态 + 断连保留最后有效样本）与重置倒计时展示
   （FACTORY_REVIEW §6），改进我们的订阅页。

**后果**：
- FACTORY_REVIEW.md 定位从"重建决策依据"调整为"交互范式与社区项目
  学习资料"；esp-brookesia 构建验证结论保留（范式可行性的证据）
- 固件路线延续现有 firmware/（BSP bring-up 完成）+ 模拟器三件套，
  新增 input 服务（三键事件）与订阅页倒计时展示

---

## ADR-004 Muse 云端连接放在 Bridge，设备端不做 Muse SDK 集成

日期：2026-11-15　状态：已接受（替代 ADR-002 中"Muse 集成按独立构建验证后合入主工程"的路线）

**背景**：用户要求 Muse 作为 launcher 里的一个 App 接入，不整包替换
固件。调研上游 `facebookincubator/muse-gadget-sdk` 后的事实：

- 上游**已内置本板**（`devices/waveshare-s3-216.md` + `sdkconfig.muse-waveshare-s3-216`），
  且作者实测通过；但整个聊天/语音代码锁在 `CONFIG_MUSE_ENABLED`（完整 UI 组件）内
- 设备侧要自建的能力：NimBLE + 社区配对 v5（约 11k 行 BLE/Noise 控制面）
  + TLS/WS + Noise + VM 语音会话；我们的固件当前 **BLE 未启用、无 TLS 使用**
- 资源现实：app 分区仅剩 245KB、内部 RAM 运行时空闲 60KB（最大块 31KB）
  → 直接移植需重排分区 + 内存瘦身，风险高且收益只是"复制一份上游 UI"
- 传输是**标准 `Noise_XX_25519_AESGCM_SHA256`**（上游 `InitiatorHandshake.h`
  明确写出），Go 有成熟实现
- 网络：本机直连 `api.muse.ai` / `hatch.metaaivm.com` 不通，经代理 7897 可达

**决策**：Muse 的 Noise 会话与回合放在 **Bridge**：

1. Bridge 用配对凭据 → `GET /api.muse.ai/fetch_vms` → `vm_id` + `vm_auth_token`
2. Bridge 建 WSS + Noise XX 会话，复用 `POST /chat/subscribe` + `/chat/stream`
3. 语音回合走**语音消息**路径（base64 WAV 进 `/chat/stream` 的 `items`）——
   `/api/voice/dictation` 对该凭据返回 403，且上游注释写明该路由暂无 ASR
4. 设备端**零新增**：Muse 页 PTT 仍打 `/api/v1/voice/interact?app=muse`，
   Bridge 内部路由到 Muse；回复文本由本项目 TTS 朗读（上游只显示文字）
5. Muse 不可用时自动回落常规 ASR+LLM+TTS 链路

**理由**：符合项目硬约束（云端凭据只进 Bridge）与 ADR-003（每生态一个
App，UI 共用）；设备端零 BLE/TLS/分区改造；全部可在 Mac 上验证。

**后果**：
- 配对仍需一次性完成：临时刷官方固件 → Muse app 配对 → 从设备 NVS
  （0x11000, 0xC000）导出 `access_token`/`refresh_token`/`node_id`/
  `noise_host` → 刷回本固件。凭据失效（revoke）需重做这一步
- Bridge 必须能访问 Muse 云端（本机用 `proxy_url` 走 Clash；部署环境需
  相应网络条件）
- 上游的 avatar/相册等 UI 能力不引入；Muse 页沿用本项目伴宠与对话 UI

---

## ADR-005 小智官方服务由 Bridge 模拟设备接入（规划中）

日期：2026-11-15　状态：**已实施**（2026-11-16 真网验证通过；PLAN §5.5）

**背景**：用户希望设备「小智」App 使用官方 xiaozhi.me 服务的 Agent——
官方人设与台湾腔「湾湾小何」音色。两条原路线都有问题：

- 设备端实现小智协议：需要 TLS/WS + Opus + 设备激活，且与本项目
  Voice Session/UI 两套"操作系统"并存；内部 RAM 与 flash 余量都不支持
- Web 调用 xiaozhi.me console 内部 API 改人设/音色：C 级非公开接口，
  不承诺稳定，且仍无法让设备听到官方 TTS

调研确认官方服务支持 **WebSocket-only PTT**：OTA 拿激活码 → 控制台绑定 →
re-OTA 拿 WS token → hello/listen start → 上行裸 Opus（16k/60ms）→
`stt`/`llm`/`tts` + 下行 Opus（通常 24k）。设备身份只需稳定的
`Device-Id`(MAC) + `Client-Id`(UUID) + User-Agent。

**决策**：在 **Bridge 模拟一台 xiaozhi 设备**，设备端协议零改动：

1. Bridge 持久化设备身份；`POST /api/v1/xiaozhi/connect` 触发 OTA，
   未绑定返回 6 位码给 Web 展示并轮询 `/activate`（202→3s，200→完成）。
2. 绑定后 re-OTA 保存 `websocket.url/token`（token AES-256-GCM 落库）。
3. 设备 `app=xiaozhi` 的 `/voice/interact` 走官方回合：PCM→Opus 上行，
   下行 Opus 解码→16k PCM，按现有 type-1 帧直通（官方 TTS，不听本地 TTS）。
4. 未配置/OTA/WS 失败自动回落现有 ASR+LLM+TTS（与 Muse 同款）。

**理由**：与 ADR-004 同构——凭据只进 Bridge、设备零协议改动、全部可在
Mac 上验证；人设/音色仍由用户在官方控制台配置，Bridge 不碰 console API。

**后果**：
- 依赖 Opus 编解码（倾向 cgo libopus；CI 加 `libopus-dev`）与官方服务
  的稳定性；协议变化/额度限制需 re-OTA 或降级
- 激活必须人工在 xiaozhi.me 输入 6 位码；一个 Bridge 身份对应一个官方设备
- 官方 TTS 音频直通后，Voice 页的「小智音色」只在回落链路生效
- 实施结论（2026-11-16）：`hraban/opus` + `-tags nolibopusfile` 构建链
  （CI 装 libopus-dev）；probe 与设备 PTT 直通均真网验证通过
