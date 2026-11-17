# 决策记录（ADR）

按时间倒序记录影响系统边界的决策。每条包含背景、决策、理由与后果。

---

## ADR-009 Bridge 部署常驻主机 + 设备运行期配网（脱离 Mac）

日期：2026-11-16　状态：已接受（用户拍板）

**背景**：Bridge/Web 一直跑在开发 Mac 上——Mac 睡眠设备就废。用户要求
设备独立运行，且设置页配网/后台入口要真能用（配网页原是写死的演示
二维码，扫码 404；Wi-Fi/Bridge 地址是编译期宏）。

**决策**：

1. **不把 Bridge 塞进设备**：ESP32-S3 跑不了 Go，且「API Key 只进
   Bridge」的安全模型不允许几十个供应商 Key 存设备。Bridge 部署到
   任意常驻主机（服务器/NAS/树莓派），零依赖单二进制+SQLite。
2. **Web 后台由 Bridge 托管**（`-webdir`）：API+管理页一个端口一个
   进程，设备「管理后台」二维码直接指向 `http://<主机>:8081`。
   Mac 只留开发用途（Vite dev server 照旧）。
3. **设备运行期配网**：设置→网络与连接 = Wi-Fi 扫描+软键盘输密码
   （存 NVS）+ Bridge 地址热改（保存即 `ds_bridge_rebind` 生效）。
   `wifi_secret.h` 退为首次出厂兜底（NVS 配过即忽略宏）。

**后果**：

- 部署链：`scripts/build-release.sh` + `docs/DEPLOY.md`（数据迁移
  必须带同一个 `BRIDGE_MASTER_KEY`，否则凭据解不开）
- 换 Wi-Fi 环境后 Bridge 不可达不再需要重刷固件——配网页改地址即可
- 安全模型不变：设备 NVS 只存 Wi-Fi 凭据与 Bridge 地址（非 API Key）

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

---

## ADR-010 设备存储：TF 卡备份/缓存 + 文件管理器；删 ZCode/服务器监控

**背景**（2026-11-16 用户需求）：app 分区只剩 5%、运行期 internal RAM
峰值紧；设备插着 TF 卡但固件挂载后零使用；ZCode 远程与服务器监控两个
App 无实际数据源（Bridge 侧配置页也常年空置）。

**决策**：
1. **删除 ZCode 远程 + 服务器监控全链路**：设备端枚举/defs/ui_monitor.c/
   图标位图/servers+Komari 数据链；Web 页面/路由/导航/音色项；Bridge
   zcode.go/monitor.go/internal/komari/设置键/音色白名单。launcher
   `readOrder()` 改为**过滤已下线 key**（旧排布不整单回落出厂重置用户
   自定义；设备端 defs 匹配本来就跳过未知 key）。收益：flash 5%→6%，
   运行期少 servers 抓取与一类 worker。
   **launcher 排布只排序、不隐藏（同日修复真机"文件 App 出现一下又
   消失"）**：旧模型 order=可见列表、缺省=隐藏，导致新内置 App（files）
   不在旧排布里时被藏掉——设备开机全量注册可见，联网拉到旧排布重建后
   消失；且"显示与否"错误地依赖 Bridge 配置版本（公网 Bridge 未升级就
   消失）。用户拍板：**文件等 App 是固件内置能力，存在性归固件**。现
   模型：设备端始终注册全部 app_def_t，远端 order 只做排序、缺的追加
   末尾；Bridge `visibleOrder()` 同样补全 catalog 并归一化保存，旧配置
   （含已废弃的 version/hidden 字段）只读 order、未知 key 过滤；Web 外观
   页去掉"隐藏/恢复"，只保留 ↑↓ 排序。
2. **TF 卡三件套**（`/sdcard`，FATFS 1-bit SDMMC）：
   - 配网自动备份/恢复：save/remove 后写 `backup/provision.json`
     （Wi-Fi+地址簿，JSON/cJSON）；开机 NVS 无凭据自动恢复（刷机/擦
     NVS 免重配）。**备份含 Wi-Fi 明文密码——TF 勿外借**（文档注明）。
   - 订阅/天气离线缓存：成功抓取整包 JSON 落 `cache/`；失败回读解析，
     订阅的"更新于"用文件 mtime——断网可见最后一次数据。
   - 自定义背景落盘改 `appearance/custom-bg.png`（修掉真机写相对路径
     必失败的 bug）。真机屏显需 LV_USE_PNG（约 +50KB flash），本期只
     落盘，屏显列后续。
3. **文件管理器 App（key `files`）**：浏览/进目录/上级/删除（两击确认）/
   上传到 Bridge/从 Bridge 下载（≤2MB，PSRAM 整包缓冲）。目录扫描与
   传输全在 16KB worker + lv_async_call，LVGL 线程零阻塞 IO（踩坑#11）。
   Bridge 新增 `/api/v1/files` 四端点（≤8MB、文件名清洗、显式
   Content-Length），Web 新增「设备文件」页——TF 与后台双向同步。
4. 内存：net_probe 栈 12→8KB；TF 相关 HTTP 缓冲统一 PSRAM 分配。

**后果**：
- localStorage 语义未变；旧 DB 的 launcher order 首次读取即被过滤，
  保存一次后彻底干净（zcode/monitor 的 app_settings 行成为孤儿，无害）
- TF 是增强项不是依赖：未插卡时文件页空态、备份/缓存静默跳过，其余
  功能不受影响
- 1-bit SDMMC 吞吐未实测（HARDWARE §2.5 TODO）；上传/下载上限 2MB
  即由此保守设定，实测后可放宽

## ADR-011 后台运行规则、TF 存储约定与内存观测（2026-11-16）

**背景**：应用组合从"远程运维"转向个人 AI 终端（聊天/小智/Muse/订阅/
天气/文件/番茄钟）。用户评审提出：番茄钟应支持长休息、统计与后台计时；
TF 卡应成为统一存储；应持续观测 RAM。本期先落地与番茄钟直接相关的
部分，并为后续音乐/聊天统一服务定规则。

**决策**：
1. **后台运行规则**（App 生命周期契约）：

   | App | 退出前台后 |
   |-----|-----------|
   | AstrBot / 小智 / Muse | 保存会话状态，worker/音频停止（on_exit） |
   | 订阅 / 天气 | 停止自动刷新 timer，保留 TF 缓存 |
   | 文件 | 关闭目录/文件句柄，worker 结果按代际丢弃 |
   | 设置 | 保存配置，释放页面对象 |
   | 番茄钟 | **后台继续计时**（lv_timer 不随页面销毁） |
   | 音乐（未实现） | 预留：独立后台播放任务（后续 ADR） |

2. **番茄钟 v2**：25 分钟专注 / 5 分钟短休息；每完成 4 轮专注进入
   15 分钟长休息。计时器是 App 级 lv_timer，`on_exit` 只清 UI 引用、
   不改运行状态，切回页面立即恢复正确剩余；阶段在后台完成时照常切换
   并落盘。统计（今日/累计）写 `<TF>/data/pomodoro.json`：加载/保存
   都在 8KB worker + `lv_async_call`（LVGL 线程禁阻塞 IO，踩坑 #11），
   写临时文件后 rename 原子替换；跨天今日数归零、累计保留。无 TF 时
   统计退化为内存值，功能不受影响。
3. **TF 目录契约**（`sd_paths.h` 单一来源，所有模块只通过
   `sd_root/sd_resolve` 访问）：`backup/` 配网备份、`cache/` 可重建
   缓存、`data/` 应用数据（番茄统计，后续聊天记录）、`appearance/`
   用户外观、`downloads/` Bridge 下载。后端集中式 Storage Manager
   暂不抽象成服务层——当前直接调用 sd_paths，等音乐/聊天历史落地时
   再统一（避免过度设计）。
4. **内存观测**：固件 `app_manager` 在每次 open/back 打印
   `appmem` 日志（free heap / 历史最低 / PSRAM free / 内部 RAM 最大
   连续块）；`main.c` 已有 `heap_report` 覆盖启动各阶段。新增功能用
   实测 delta 判断成本，不靠估算。模拟器不打印（无 ESP heap）。
5. **明确暂缓**：统一 Chat Service（当前三 App 共用 conversation 但
   只有一个前台会话，不存在多 TLS 常驻）；音乐后台播放（占位 App）；
   完整 Resource Manager（PSRAM 配额/图片/音频统一池）——音乐落地时
   一并设计。

**后果/边界**：
- 后台计时在设备重启/深睡后丢失（未做持久化恢复）；页面重开从内存
  状态继续，统计已落盘
- 阶段切换暂无声音/振动提醒（固件无通用 tone API）；回到番茄钟页面
  可见新阶段和统计变化，后续接通知服务
- 统计文件极小（<200B），TF 写频率最多每个专注周期一次

------------------------------------------------------------------------

## ADR-012 esp-brookesia 复评：不引入框架，借鉴三个机制（2026-11-16）

**背景**：用户确认「出厂固件 = Brookesia Launcher + 多应用」，提出
能否复用其 Phone/Launcher/App Registry/生命周期。ADR-007/008 已做过
一次路线取舍（2026-11-14，最终回到自研）；本次用当前仓库重新实测，
完整数据见 [BROOKESIA_EVAL.md](BROOKESIA_EVAL.md)。

**复评事实**：
- `03_esp-brookesia` 示例在 IDF v6.0.3 + BSP 2.0.1 + LVGL 9.4 下
  编译通过（1937 单元），但内容只有 `brookesia_core`（Phone/Speaker
  壳）+ 1 个 Squareline demo；出厂 9 个 App 不开源。
- 示例 bin 2.43MB（4MB 分区余 39%）：brookesia_core 631KB、
  libstdc++ 203KB、demo 268KB、LVGL 1.04MB。
- 本仓库固件 3.98MB，4MB 分区仅余 **216KB（5%）**；出厂固件 app
  镜像 5.39MB、分区 8MB。
- 迁移面覆盖全部 UI 层（9 App 改 C++ 类、Launcher/状态栏/手势/
  主题重写、数据层服务化），且圆屏/定稿 UI/性能规避要全部重验。

**决策**：
1. **维持 ADR-008 的自研固件基线，不引入 esp-brookesia 依赖**。
   框架复用带来的收益（现成 Launcher/Recents/生命周期）抵不过
   +0.8~1.1MB flash、C++ 运行时和全 UI 迁移风险。
2. **借鉴三个机制**（不改框架）：
   - P1（**已落地 2026-11-16**）：`ui_statusbar_set_app_status()` 单
     label 药丸；番茄钟后台显示 `🔔 24:31`，前台/暂停隐藏。实测
     flash +768B、内部 RAM <1KB、无新任务/栈。
   - P2（**已落地 2026-11-16**）：`app_def_t` 末尾增加
     `APP_FLAG_BG_TIMER/STATUSBAR/KEEP_STATE`，番茄钟标
     `BG_TIMER|STATUSBAR`，`appmem` 日志带 `flags=0x3`。
   - P2（**已落地 2026-11-16**）：`app_manager_timer_create/delete`
     登记重复 timer，切走 App 时统一清理；订阅页两个 timer 已改用该
     接口，`on_exit` 只断引用。模拟器验证
     `[app] timer cleanup app=0 left=0`；一次性/后台常驻 timer 不登记。
3. **重评触发条件**：产品需要第三方 App 动态安装/应用商店/JSON UI/
   多运行时。届时评估 upstream v0.8（支持 IDF 6.0–6.2），以独立
   工程/分支验证，不在本仓库渐进替换。

**后果**：
- flash 预算：P1 +768B；布局重做 + P2 + 移除未用的
  `font_pomo_16` + 订阅 worker 修复后，当前余 **221,328B / 5%**。现有
  9 App 与稳定性成果保留。
- 出厂固件仍只作恢复镜像与交互范式参考；其 App 实现不可复用。
- 框架能力（Recents/快照/App Store/服务框架）继续以「设计参考」
  身份保留在 FACTORY_REVIEW/BROOKESIA_EVAL，不产生代码依赖。

## ADR-013 分区重划：砍 OTA 双槽，factory 扩到 12M（2026-11-16）

**背景**：小智直连官方（ADR-014 筹备）需要 TLS+WS+Opus 约 +350~450KB
flash，原 app 分区只剩 ~219KB（5%）。分区表里 factory 4M + ota_0 4M +
ota_1 4M 三槽是项目第一版固件骨架照抄 ESP-IDF 双 OTA 模板预留的——OTA
功能从未实现（设备明文无鉴权 HTTP，不适合在线刷写），ota 槽从未写过
一个字节，bootloader 一直只启动 factory。

**决策**：合并三槽为 factory **12M**（0x10000 + 0xC00000 = 0xC10000，
正好衔接 assets 分区）。放弃 OTA 能力（恢复手段 = USB 线刷，本来也是
唯一途径）。nvs/assets/storage 偏移全部不变。

**实测**（2026-11-16 刷机验证）：bootloader 正常读新表并从 0x10000
启动；NVS Wi-Fi 凭据无损（启动日志 `creds=nvs`）；音频/Wi-Fi/remote
config 全链正常。app 分区余量 219KB → **8,358,608B（68%）**，后续
加 Opus/TLS/WS 及新功能不再受 flash 束缚，字体/图标无需裁剪。

**代价**：永久放弃 OTA（若未来要做，需重新规划分区并线刷一次新表）。

## ADR-014 小智设备直连官方服务（2026-11-16 立项）

**背景**：小智语音现走 设备→Bridge→官方 中转。Bridge 部署在美国 VPS
时，音频链路绕行 ~440ms 单程 + 晚高峰国际链路抖动 → 播放断续。官方
固件本就是设备直连（OTA 激活 → WSS → Opus 16k/60ms 帧）。

**决策**：设备固件增加 TLS（esp_crt_bundle）+ WebSocket
（esp_websocket_client）+ Opus 编解码（PSRAM 放编解码器状态），小智
App 直连官方；Bridge 中转链保留为回退。激活身份存设备 NVS（官方
6 位码流程，设备屏幕显示码）。详细设计另行文档化。

**关联**：ADR-005（Bridge 模拟设备的实现保留作协议参考与回退）。

**结果（2026-11-17 补记）**：OTA 激活与 WSS 握手真网跑通；Opus 上行后出现
概率性内核数据损坏，与坑 #14 的实验变量纠缠未收敛 → **暂停接线，回滚
Bridge 链**（ADR-015）。源码存档 firmware/main/xz_*.c/h（未编译）。

## ADR-015 小智直连暂停，维持 Bridge 中转（2026-11-17）

**背景**：ADR-014 直连按官方协议全字段实现并逐级验证：OTA+激活（bound=1）、
WSS 握手、Opus 编码上行全部跑通。此后每轮 PTT 都复现"思考完即崩"：
内核对象被越界写坏（xSuspendedTaskList.pxIndex 清零、SPI 总线锁结构坏、
LVGL 渲染线程 __stack_chk_fail），panic handler 二次进入。

**排查结论**：崩溃窗口与内存压力强相关（对话页稳态内部最大块 7.7KB），
且排查期间引入的实验变量（关 SPIRAM_FETCH/RODATA、LVGL 栈迁内部、内存
策略对齐官方）各自引爆了独立的崩溃模式（Cache error / flash assert /
Wi-Fi RX 分配不足开机循环）——真因与缓解手段纠缠，一晚无法收敛到唯一
根因。本板与官方 xiaozhi-esp32 板的内存画像差异（DIRAM 静态占 69%、
桌面稳态内部最大块仅 8.7KB）是结构性约束：直连所需 TLS+WS+Opus 的
运行时内存与显示/UI/伴宠共存无裕度。

**决策**：小智维持 Bridge 中转（ADR-005 链路，真网稳定）。直连源码
（xz_identity/ota/opus/session）保留在 firmware/main/ 不参与编译，
文档与证据索引见 docs/XIAOZHI_DIRECT.md。重启项目的前置条件：①真机
连续一晚无崩溃的内存水位改善（App 后台策略收紧或裁剪伴宠/绘制占用）；
②堆巡检/栈水位探针已内置（log_mem），可直接复用。

**教训（写给未来的自己）**：全局性配置（内存策略/cache 开关）一次只动
一个变量并让真机 soak 过夜；用户陪测的成本远高于多刷几轮的成本，出现
第二类崩溃模式时立即止损回滚，不要叠实验。


## ADR-016：本地音乐播放器（Hi-Fi 数字唱片机，WAV+MP3）

**日期**：2026-11-18。**状态**：已实现（真机待用户验收）。

**背景**：用户拍板做本地音乐播放器（TF 卡 `music/` 目录），蓝紫霓虹风
格与全局深色 AMOLED 统一；FLAC 明确不做（v1）。

**架构决策**：
1. **双端后端注入**：`music_player.c`（状态机/曲库/ID3/RIFF，双端公共）
   经 ops 结构注入平台后端——固件 `music_backend_esp32.c`（esp_audio_codec
   simple dec 流式 + esp-dsp FFT），模拟器 `music_backend_sim.c`（dr_mp3
   单头库 + SDL 音频输出，headless 跳过 SDL init 防 CoreAudio 挂起）。
   PCM 契约一致，UI 无感知。
2. **解码任务**：`xTaskCreateStatic` **栈放 PSRAM**（16KB，坑 #26——
   内部 RAM 是坑 #19 红线，.bss 栈曾把 Wi-Fi init 顶下线 abort 循环）；
   解码路径 fread(SDMMC)/I2S/FFT 不 disable flash cache，不踩坑 #14 断言。
   单任务串行 read→decode→write，esp_codec_dev_write 阻塞背压天然节流，
   无环形缓冲。
3. **音频输出 owner 仲裁**（audio_esp32.c）：音乐 `media_acquire` 持有期
   间——语音回复静音（`vs_notify_play_done` 立即完成，流程不卡）、提示音
   丢弃、PTT 拒绝+提示。release 时 `esp_codec_dev_close+open` 切回 16k/mono
   （1.5.x 无 set_fs）。**v1 不做 PTT 自动暂停恢复**（时序复杂度，留后续）。
4. **显示**：SW transform 整图旋转不可行（模拟器 sample 实测
   lv_draw_sw_transform 占满主线程，HARDWARE §2.7 教训成立）——唱片用
   **纯 LVGL 对象绘制**（同心纹理 arc 10 圈不均匀缺口 + 中心封面圆）+
   **24px 高光点绕行**模拟反光旋转（零 transform 成本）+ 进度弧 DRAW_POST
   自绘（订阅页同款）。弃用 RGB565 纹理图方案（静态 dsc 在模拟器渲染
   黑屏未解，绕开）。
5. **曲终事件异步化**：eos 由解码线程置标志、UI timer 经
   `music_player_get_progress` 轮询转发——**解码线程内直接调 on_track_end
   会 join 自己死锁**（play 链路 stop 等 s_run 清零）。
6. **素材零 rodata**：音乐功能未增 1 字节 rodata（纹理图方案已弃）。
7. **组件裁剪**：esp_audio_codec sdkconfig 只留 MP3 解码器（全家族默认
   开启时静态/链接体积大）；esp_codec_dev 1.5.11 无 set_fs 用 close+open。

**验证**：模拟器 headless 截帧（播放页/信息行/频谱/进度弧/药丸全通，
GADGET_SNAP + GADGET_DEMO_MUSIC_PLAY=1）；regress-sim 9 App 唯一画面
PASS；固件 post-ui internal 64.7KB（>56KB 底线）、Wi-Fi 正常、心跳正常。
真机播放/频谱/后台药丸待用户验收（TF 放 `music/` 目录 MP3/WAV）。

**已知边界**：MP3 时长按码率估算（VBR 不准）；无 seek；GBK 编码 ID3
标签显示回退文件名；PTT 自动暂停恢复未做。

---

## ADR-017：本地小说阅读器（TXT，2026-11-18）

**需求**：TF 卡 TXT 小说阅读，书架/阅读/目录/设置四页，深色 AMOLED+
蓝紫霓虹（设计图 `assets/ui/霓虹小说阅读器界面.png`）。核心四件套：
快速打开、流畅翻页、中文不乱码、断点续读。

**决策**：
1. **大文件策略**：全书不进 RAM——16KB 流式读缓冲（worker 内）+
   文件 byte offset 分页/章节表（PSRAM 惰性数组）。页边界恒为行边界，
   进度保存 byte offset（非页码），排版参数（字号/行距）变化后页表
   自动重建。
2. **编码**：BOM → 严格 UTF-8 校验（首 64KB，PSRAM 缓冲）→ GBK 兜底。
   GBK 换码用 **GB2312→Unicode codepoint 矩阵表**（87×94 uint16，
   O(1) 查表，16KB rodata，`scripts/gen_gbk_table.py` 生成）。GBK 扩展
   （繁体/生僻）单字降级 '?'。**表存 codepoint 不存 UTF-8 字节**——
   第一版存字节被调用方二次编码产生 Hangul 乱码（单测定位）。
3. **章节**：流式全文扫描 `第X章/回/卷/节/篇/集/部`、Chapter N、
   序章/楔子/尾声等特殊行（行长 <100B 限制防误判）；无章节 TXT 单
   "全文"章。章节上限 512。
4. **排版**：LVGL 字体度量逐行填充；**段首全角空格缩进**（U+3000 需
   入字体子集，gen_cn_font.sh 保险集已补）+ 行首禁则标点（拼回上一
   行尾）+ 行尾开括禁则；字号 16/20/24 三档复用现有全集主库（**零新增
   rodata 字库**，坑 #25 红线）。
5. **进度/配置**：`data/reader/progress.json`（per-book byte offset+
   章+页+unix 时间戳，temp+rename 原子写）+ `config.json`（字号/行距/
   主题/自动翻页）。书架按最近阅读排序。**不修改原始 TXT**。
6. **交互**：点击左 1/3 上一页/右 1/3 下一页/中央菜单 overlay
   （目录/设置/书架）；KEY3 短按=下一页（App 注册 handler，on_exit
   置 NULL）；BOOT=返回。阅读页零 chrome，正文 16 行 label 复用
   （setText，无重建）。
7. **字体子集重生成注意**：gen_cn_font.sh 重跑会全量重写 5 档（生成器
   格式漂移大 diff）——本次为补 U+3000 全量重生成（+U+3000 一字，
   GB2312 全集保持），regress 10 App 唯一画面 PASS 验证渲染无损。

**验证**：独立 C 单测（/tmp/rdtest，链接 reader_book+gbk_table+字体，
UTF-8/GBK 双书 18/12 章识别 + 禁则排版全对）；模拟器 headless 截帧
（书架三书列表/阅读页 UTF-8+GBK 全对）；regress 10/10；check.sh 全绿；
真机刷入开机健康。真机翻页/进度续读/大书扫描时长待用户验收。

**已知边界（v1）**：无搜索/书签/图片封面；UTF-16 文件提示不支持；
单文件 >512 章截断；自动翻页实现但无速度滑条（三档 5/15/30s）；
文件管理器联动打开 TXT（ADR-018）待做。
