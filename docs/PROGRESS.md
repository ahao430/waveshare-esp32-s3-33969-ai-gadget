# 进度对照表（PLAN §18 验收标准逐项核对）

> 最后更新：2026-11-16。状态标记：✅ 完成 · 🔶 部分完成 · ⏳ 未开始 ·
> 🔒 等外部输入（解锁条件注明）。证据见 [DEVELOPMENT.md](DEVELOPMENT.md)。

## Phase 0：项目初始化 ✅

| 验收项 | 状态 | 说明 |
|--------|------|------|
| 阅读 Waveshare 官方仓库/Wiki/BSP | ✅ | HARDWARE.md（BSP 源码交叉确认引脚表） |
| 确认 BSP 版本 | ✅ | `waveshare/esp32_s3_touch_amoled_2_16 ^2.0.1` |
| 确认 ESP-IDF 主版本 | ✅ | v6.0.x（ADR-004；实装 v6.0.3） |
| 检查 03_esp-brookesia 示例 | ✅ | 依赖极简（BSP 单依赖）；结论进 HARDWARE.md §3 |
| 创建 Monorepo | ✅ | 五目录结构（PLAN §17） |
| Firmware 最小工程 | ✅ | IDF v6.0.3 编译通过（0.6MB，85% 分区余量） |
| Bridge 最小工程 | ✅ | 9 个测试包全绿 |
| Web 最小工程 | ✅ | vue-tsc + vite build |
| 代码规范 | ✅ | 各工程风格约定（docs 沉淀） |
| CI 编译检查 | ✅ | scripts/check.sh 本地全绿 + GitHub Actions（.github/workflows） |

## Phase 1：PC UI ✅

| 验收项 | 状态 | 说明 |
|--------|------|------|
| LVGL PC Simulator 运行 | ✅ | LVGL 9.2.2 + SDL2，480×480 |
| 480×480 主题 | ✅ | 深色 OLED 友好（ui_theme.h = Web tailwind 同源色板） |
| Launcher | ✅ | 状态栏/网格/页面指示器/时钟 |
| App Manager | ✅ | 注册表/单前台/退出即毁（auto_del） |
| Mock 硬件层 | ✅ | data_source mock/bridge 双实现（ADR-001） |
| 页面跳转和返回 | ✅ | 转场动画；语音/聊天/监控页实装，其余占位 |
| 资源释放测试 | ✅ | on_exit 停定时器/租约；代码评审 + 运行验证 |
| （超额）CJK 字体 | ✅ | Noto Sans SC 子集管线（gen_cn_font.sh） |

## Phase 2：Web 管理后台 ✅

| 验收项 | 状态 | 说明 |
|--------|------|------|
| Vue3+TS | ✅ | 严格模式 typecheck |
| 手机/PC 响应式 | ✅ | <640px 顶部导航（§9.1）；真机浏览器待实测 |
| Dashboard | ✅ | 设备总览 + 额度卡 + 实时刷新 |
| Wi-Fi 页面 | ✅ | Mock 设备 API（接口签名按 §9.4 预定） |
| 代理配置 | ✅ | Direct/HTTP CONNECT（§10.2） |
| 设备设置 | 🔶 | API 已预留（config），UI 未做 |
| 二维码配网原型 | ✅ | 三步向导；二维码只含 URL（§9.3） |
| API Mock | ✅ | device.mock.ts |
| 认证与脱敏 | ✅ | Bridge Token（401/200 实测）；Key 不回显 |

## Phase 3：Bridge ✅（收官）

| 验收项 | 状态 | 说明 |
|--------|------|------|
| Go HTTP API | ✅ | 标准库 net/http（ADR-002） |
| WebSocket | ✅ | /api/v1/events；quota/komari 推送（2 组集成测试） |
| SQLite | ✅ | 快照持久化（重启恢复实测）+ 凭据表 |
| 配置加密 | ✅ | AES-256-GCM；DB 无明文实测；fail-safe 缺 key 禁用 |
| Provider 接口 | ✅ | quota.QuotaProvider（§6.1 契约） |
| 智谱 Adapter | ✅ | C 级；fixture 单测；🔒 真实账号验证待 Key |
| Mock Provider | ✅ | 伪随机确定性数据 |
| 额度快照和错误状态 | ✅ | stale/last_error/UnsupportedResponse 语义 |

## Phase 4：订阅与监控 App 🔶

| 验收项 | 状态 | 说明 |
|--------|------|------|
| 智谱额度 | ✅ | 模拟器+Web 双端显示；真实数据 🔒 待 Key |
| 手动预热 | ✅ | Bridge 状态机 6 测 + Web/模拟器交互 |
| Kimi Code | ✅ | Bridge Adapter（B 级）；模拟器页接双 provider |
| MiniMax | ⏳ | P2，PLAN 允许后置 |
| Komari | ✅ | Bridge 聚合 + Web/模拟器监控页；🔒 真实部署验证 |
| ZCode Remote | ⏳ | 🔒 数据源待拍板（本机 DB or 远程 API） |
| 验收标准（异常/无额度/重置/未知字段呈现） | ✅ | mock + fixture 覆盖四类场景 |

## Phase 5：AI Chat 与语音 🔶

| 验收项 | 状态 | 说明 |
|--------|------|------|
| Bigfish Chat 文字 | ✅ | Bridge Router + Web/模拟器双端 |
| 流式响应 | ✅ | SSE（Web fetch 流式 / 模拟器 chunked 解析，46-chunk e2e） |
| Audio Manager | ✅ | voice_session 状态机 + ops 抽象 + audio_mock |
| 语音采集 | ✅ 协议 | interact 一步式（PCM→WAV）；🔒 真机 I2S 待设备 |
| TTS 播放 | ✅ 协议 | WAV stub 生成；🔒 真机 ES8311 待设备 |
| Muse 独立验证 | ⏳ | 🔒 等设备 + Muse SDK 独立构建（材料齐：HARDWARE §3） |
| Xiaozhi 独立验证 | ⏳ | 🔒 协议文档+骨架设计齐（XIAOZHI_PROTOCOL.md）；等端点 |
| HTTP CONNECT 代理 | ⏳ | 设备侧网络栈随 Phase 6 |

## Phase 6：33969 真机适配 🔒 全部等设备

烧录/i2cdetect 已前置进骨架固件；逐项执行清单见
[BRINGUP.md](BRINGUP.md)（显示→触摸→音频→TF→PMU→RTC→IMU→按键→睡眠→Wi-Fi
+ Muse 坑 checklist 6 条 + 记录模板）。

## Phase 7：优化 ⏳

依赖 Phase 5/6 的真实链路；性能模板已备（BRINGUP.md §3）。

## 汇总

- **PC 侧可推进项：全部完成**。
- 等待点：真机设备、真实 Provider Key、Muse/小智服务端账号。
- 仓库存档：30+ 逻辑提交（docs/bridge/simulator+firmware/web/chore）。

## 增量交付（2026-11-14 设备到手后）

| 交付 | 状态 | 位置/说明 |
|------|------|-----------|
| 出厂系统架构审查 | ✅ | FACTORY_REVIEW.md：出厂源码闭源；esp-brookesia 范式学习；ADR-007/008 定稿自研路线 |
| 电源：状态栏/设备页/power_manager | ✅ | ui_statusbar 五档+充电/USB 语义；AXP2101 组件骨架（寄存器待 bring-up） |
| 电源管理方案 | ✅ | POWER_MANAGEMENT.md（前台独占/按能力后台/功耗测试 T01-T13） |
| 订阅实例动态配置 | ✅ | Bridge subscriptions CRUD+聚合快照；Web 配置页；设备空态/横滑 |
| AstrBot 对接 | ✅ | 大肥鱼对话路由 bot 实例（SSE 适配层）；Web kind 下拉 |
| AstrBot 思考链过滤 | ✅ | 实测事件协议：思考与正文同为 type=plain 增量，区分字段是 chain_type（reasoning=思考 / null=正文）——旧的 type 白名单从未命中，思考一直被当正文念；三条消费路径（文字流式/语音流式/语音非流式）统一按 chain_type 过滤。真网 e2e：同一提问修复前 140+ 条英文思考事件，修复后设备收到 177 汉字 0 英文字母 |
| Muse App 接入 | ✅ | 云端连接在 Bridge（ADR-004）：配对凭据 → fetch_vms → WSS+Noise XX → /chat/subscribe + /chat/stream；语音走语音消息（base64 WAV）；回复由本项目 TTS 朗读。app=muse 无配置时自动回落常规链路。真网实测：转写「你好，用一句话介绍你自己。」+ 用户人设回复 + 9.5s 朗读音频；阶段 0 一次性配对流程（刷官固件 → Muse app 配对 → NVS 0x11000 导出 → 刷回）已跑通 |
| 交互范式统一 | ✅ | 一屏四格横滑+页点；上滑返回；按键短按映射（ui_keys 分发）；ui_gesture 公共件 |
| 素材管线 | ✅ | PNG 序列帧 290 帧（companions/ 规范目录）；companion 帧播放器 |
| 语音崩溃修复 | ✅ | chunked 块 >8KB 写爆栈（read_chunked_body 分片）；GADGET_DEMO_VOICE 回归钩子 |
| 官方教程索引 | ✅ | WAVESHARE_GUIDES.md（烧录/ESP-IDF/Arduino 对照） |
| 重置倒计时 | ✅ | 订阅页 1s 动态倒计时（QuotaFrame 借鉴） |
| 机器人选人设/模型（AstrBot） | ✅ | bots 表扩 config_id/provider/model（ALTER 迁移）；chat 请求始终带 config_id（default 重置粘滞路由）+ selected_provider/model 覆盖；/api/v1/bots/configs 代理拉配置文件列表（403→提示补 config 权限）；Web 表单下拉+编辑（token 缺省=保留） |
| 主页排布（Web 配置） | ✅ | /api/v1/launcher（order=可见顺序，不在列表=隐藏；目录+校验）；设备 app_def_t 加 key、启动拉取重排；Web 排布页（↑↓/隐藏/恢复/分页预览）；e2e：设置提到第一+隐藏 3 个 → 桌面顺序与页点（2 页 2 点）像素验证 |
| 天气 App 全链路 | ✅ | Open-Meteo 免 Key（IP 定位 ip-api/ipwho.is + 手动城市 Geocoding）；Bridge 缓存 10min+保温；Web 配置页（自动/手动）；ui_weather 页面（当前+5日）；字体重生成（°+天气字典+GB2312 一级字库治动态中文豆腐） |
| 应用配置四域（机器人/ZCode/监控/按键） | ✅ | Bridge bots 表+app_settings（astrbot 订阅自动迁移）；/bots CRUD+probe、/zcode config+sessions、/monitor config+动态源、/keys；Web 四配置页；模拟器按键映射拉取+KEY3 长按 PTT（A/B 快照验证 Web 下发生效） |
| 主页排布真机生效 | ✅ | 根因：真机 ui_init 时 Wi-Fi 未起，拉不到 Web 排布且 GOT_IP 后无人补拉。修复：app_manager_clear+register_apps 可重跑；remote_cfg_task 两段式（prefetch 不持锁抓 theme/bg/keys/launcher，refresh 持锁应用+重建桌面）。附带修复 ui_launcher_rebuild 两颗雷：g_page_dots 悬垂指针（重建即 LoadProhibited 崩溃）、波浪背景 buffer 重建泄漏。真机验证：好启动 6.5s 内 layout applied + remote config applied + 探针 FREE。启动期 wedge 为先前已存在问题（HARDWARE.md §2.10 归因） |
| Web 后台信息架构重排 | ✅ | 主页排布并入外观；网络页合并首次配网+后台二维码（当前地址码+局域网演进说明）；代理独立 tab；聊天机器人→AstrBot；模型 API→语音设置（/voice，ASR 卡片加"去获取"直达官方控制台，音色表加 AstrBot 项跳实例设置）；Muse/小智独立 tab（小智提供 xiaozhi.me 控制台入口）；订阅中心每实例"去获取"（有 base_url 跳站点、否则官方地址） |
| AstrBot 多实例切换 | ✅ | 根因：设备/Bridge 无"当前机器人"概念——路由空 model 回落永远取 `ListBots()[0]`（id 排序），设备端也无切换入口，配了 3 个只生效一个。修复：app_settings.chat_active_bot + storage.ActiveBot（失效自动回落第一个，删当前实例自动清设置）；BotResolver 把当前实例排首位（chat 文本/语音都跟随）；TTS 偏好跟当前实例。接口 GET/PUT /api/v1/chat/active；Web AstrBot 页「设为当前」+ 当前徽标；设备 AstrBot 页点标题切到下一个实例（后台线程，标题显示当前机器人名）。实测：3 实例下 PUT 切换 → 空 model 文本回合人设变化（大肥鱼 vs 魅魔），storage/api 均有回归测试 |
| 首页页点高亮不跟随 | ✅ | 根因：设备桌面禁拖动、翻页走"快滑"回调直接 set tile，且先把缓存页号 s_last_page 改掉；SCROLL 回调按页号缓存节流 → 永远短路，页点停在第 1 页。修复：翻页收敛到 launcher_goto_page（set tile + 页点高亮 + 缓存页号），快滑与 GADGET_DEMO_PAGE 共用。像素验证（headless 三页截图）：y=450 高亮段分别为 211-228 / 231-248 / 251-268，色 #7c5cff（其余 #46527a） |
| AstrBot 设备端选机器人 | ✅ | 点 AstrBot 页标题弹「选择机器人」全屏面板（列全部实例，当前项 ✓ + 强调色；后台线程拉列表/PUT，不占 LVGL 任务）。动态机器人名豆腐修复：新增 GB2312 二级字库回退字体 font_cn_dyn_14（3008 简体字 / 14px / bpp2，约 150KB），挂到 16/20px 主字体 fallback 上；gen_cn_font.sh 负责生成+挂接（注意生成器自带 `.fallback = NULL` 会覆盖，必须替换该行；extern 要放在 include 之后且不在 #ifdef 分支里）。headless 截图验证「魅魔/萧薰儿」正常渲染。**flash 余量告警：app 分区仅剩 65KB（2%）——下次加功能需先做字体压缩或分区调整** |
| Flash 余量治理 | ✅ | 未压缩字体把 app 分区吃到 2%（65KB）。开启 LVGL RLE 压缩字体（固件 CONFIG_LV_USE_FONT_COMPRESSED + 模拟器 LV_USE_FONT_COMPRESSED，gen_cn_font.sh 默认压缩）→ 3.63MB、余 552KB（13%）。坑：lv_font_conv 默认即压缩，`--compress` 参数不存在；未开解码开关时中文渲染空心方块（headless 截图实测）。check.sh 增加余量打印 + <5% 告警。PLAN §14.4 记录预算与后续手段 |
| 设备端选机器人面板修复 | ✅ | 真机点标题崩溃根因：拉列表 worker 只给 12KB 栈，实测用掉 11.4KB（`stack free=12640`@24KB）——12KB 必溢出。修复：栈升 24KB + 面板构建改 `lv_async_call` 回 LVGL 线程（跨线程建对象此前风险高）+ 建成日志。真机自检实证：`bot picker fetch rc=0 n=111` → `bot picker shown: 5 bots`，LVGL 持续存活、无断言 |
| 显示管线楔死自愈 | ✅ | 深挖：楔死时 lvgl 任务 Blocked、双核 idle；hangprobe 会卡在 `esp_lv_adapter_lock`（0 超时也一样）；移除探针对照仍 4/8 楔死 → 探针非诱因。已加任务看门狗（TWDT 15s + PANIC）→ 楔死后 ~28s 自动重启自愈，无需人工断电（实测日志 rst:0xc）。root-cause 待抓阻塞 backtrace |
| “一说话就重启”根因 | ✅ | 真机日志：语音 POST 期间 `Failed to allocate priv TX buffer` → `Draw bitmap failed: NO_MEM` → LVGL flush 卡死 → 看门狗重启。根因：draw buffer 在 PSRAM，SPI 每次发送要在内部 RAM 分配同尺寸 bounce buffer（480×20×2=19.2KB）；语音起始（16KB 栈+HTTP）把内部 RAM 压到 ~39KB 且碎片化。**与字库/压缩无关**。修复：分块 20→10 行（9.6KB）、hangprobe 栈 8KB（4KB 实测栈溢出）、WiFi/LWIP 缓冲允许落 PSRAM。回归自检：33280B 语音请求完成，零 NO_MEM |
| “回答时重启”收敛 | ✅ | 三轮反复后的最终配置：绘制缓冲回 PSRAM+20 行、WiFi/LWIP 缓冲入 PSRAM（内部 RAM 84K）、hangprobe 8KB、字体不压缩不带 L2 回退（用户决策）。设备自检完整 PTT 回合（听→想→说→待机）通过：零 NO_MEM、零 panic、heap 42→60K。**中途踩的坑已记录**：绘制缓冲搬内部 RAM → 语音线程 16KB 栈 ENOMEM（状态卡"思考中"）；线程栈放 PSRAM → InstrFetchProhibited 栈损坏；10 行分块 → 每帧 48 次 bounce 分配更易碎片化 |
| 花屏+重启彻底修复 | ✅ | 关键认知：绘制缓冲放 PSRAM 时，SPI 每块要在内部 RAM 临时分配 bounce buffer，聊天页打开后内存碎片化必然失败 → 丢帧=花屏 + flush 卡死 + 看门狗重启。最终方案：**绘制缓冲开机放内部 RAM（10 行双缓冲 19.2KB，DMA 直连零拷贝）+ 语音线程改常驻静态栈任务（.bss 14KB）+ http 读缓冲入堆 + WiFi 缓冲入 PSRAM**。设备自检连续 6 轮完整语音回合全通：零 NO_MEM、零 panic、栈余 4748B |
| 音色试听（Web 后台） | ✅ | 语音设置各 App 音色、AstrBot 实例音色旁加「▶ 试听」：POST /api/v1/voice/preview（provider/voice → WAV 试听样例），不落配置、不读写 TTS 冷却、失败直接回显真实原因（未配 Key/音色不可用）；自动档按设备实际路由顺序试。真网验证：Edge 云希 4.10s/131KB（RMS 1575）、DeepSeek stella 102KB；火山未配 Key 返回「火山 TTS 未配置」提示 |
| 小智官方服务接入 | ✅ 真网端到端打通 | ADR-005 落地：internal/xiaozhi（OTA/激活/WS 回合/Opus 编解码 hraban/opus+libopus，-tags nolibopusfile）+ voice 双路径集成（stream 官方音频直通 type-1 帧、非流式整段 WAV；早失败回落本机链路）+ Web 小智页（连接/激活码/绑定轮询）+ cmd/xiaozhi-probe。**真网实测**（2026-11-16）：OTA 注册 → 控制台输码绑定 → probe 回合「小智你好，请用一句话做个自我介绍」→ 官方转写全对、回复官方台湾人设「我是小智呀，一个来自台湾…」、5.46s 官方 TTS（RMS 3363）落盘；设备端 PTT 直通 `app=xiaozhi` 103 PCM 帧/193KB/字幕 32 字零错误，Bridge 分段 7.4s |
| AstrBot 会话隔离与「新对话」 | ✅ | 根因×2：①会话轮换代只存内存——Bridge 重启退回旧代，服务端旧记忆"复活"；②全实例共用 username=gadget，AstrBot 用户级记忆/绑定按 username 共享（会话键 webchat:...:webchat!{username}!{session_id}）。修复：轮换代持久化（app_settings.astrbot_sessions，重启实测 2→3 接续）；每实例独立 username（gadget-<botID>，重置时 -r<gen> 连用户级记忆一并清）；Web AstrBot 页每实例「新对话」；设备端聊天记录按机器人分槽（PSRAM 8×12 条）、「新对话」只重置当前机器人（其余 App 清空不再误触服务端重置）。设备侧已随 2026-11-16 固件刷入，后续多轮真机使用无回归报告 |
| PWR 键关机 | ✅ 配置生效 | 根因：固件从未配置 AXP2101 PWR 键，行为看 PMU 出厂默认（16s 关机位有的芯片出厂开、长按时长不确定）。修复（寄存器照抄本板实测的 Muse muse_pmu.c）：长按 10s 硬关兜底 + 键事件状态位轮询（100ms）+ 长按即优雅关机（灭背光→REG10 bit0 软关）。HARDWARE §2.9.1；真机开机日志实证 `PWR key armed: 10s hard-off fallback, events on`（USB 在位时软关后可能立即重新上电，属硬件行为；长按关机手感待用户确认） |
| 连续对话（免提） | ❌ 已移除 | 三轮真机修复后仍不稳（VAD 触发/时序随环境漂移，用户 2026-11-16 拍板移除）。移除范围：左上角按钮、KEY3 短按注册、VAD/自动轮 poll 驱动全部拆除；KEY3 长按=语音说话保留。**保留的有效遗产**：①vs_cancel API（Listening→Idle 干净取消，通用）；②采集任务常驻（修 ES7210 RX DMA 溢出回零——手动按住模式同样受益，此前的"循环同一句"根因）；③背光持显示锁串行化（修开机与首帧 DMA 抢 QSPI 总线的断言崩溃） |
| 订阅中心重构（实例即一切） | ✅ | 用户拍板：砍掉独立「额度查询凭据」区——订阅实例=类型+名称+端点+Key 单一概念（"配了凭据没建实例→设备订阅页空白"的两层概念踩坑即源于此；设备订阅页只读 subscriptions 表，此前为空）。Web：kind 切换自动填官方默认 Base URL（中转站类必填校验保留）、列表支持编辑（Key 留空=保留）、新增必填 Key。Bridge：保存实例把 Key 投影到凭据表（语音 ASR resolveAccount 继续动态解析同一份，凭据区删除不伤语音）、实例无 Key 抓取时回落凭据表、POST 同 id=编辑 upsert。存量 zhipu/deepseek 凭据已迁移成两个实例（真网验证额度抓取 OK；用户手建的两个无 Key 重复实例已清） |
| 设备「管理后台」二维码 | ✅ | 设置→网络与连接的二维码原是写死演示地址 192.168.1.42（用户扫码 404）。新增 ds_admin_url（data_source 三件套 + bridge 实现从基址剥 host 拼 Web :5173，同机部署假设/Vite host:true）；未连接 Bridge 弹提示不渲染空码。已刷机，启动探针 FREE |
| 设备运行期配网 | ✅ | ADR-009：设置→网络与连接重做——Wi-Fi 设置=全屏面板（esp_wifi 扫描/同名去重/软键盘 lv_keyboard+lv_textarea 输密码/失败恢复旧网重连/成功存 NVS）；Bridge 地址=热改（保存即 ds_bridge_rebind 重算全部 URL，无需重启）；状态卡 1.5s 轮询真数据。新组件 firmware/components/wifi_prov（NVS+扫描+试连事件组等待）；data_source_wifi.c 单文件双平台（模拟器演示数据+GADGET_DEMO_WIFI 场景）；main.c wifi_start NVS 优先（启动日志 creds=nvs/wifi_secret.h 标来源）、wifi_evt 配网态避让自动重连、未配网也 start 供扫描。headless 截帧验证列表/密码视图/键盘渲染（GADGET_DEMO_APP=8 注意是 UI_APP 枚举值非注册序号）；已刷机启动链正常 |
| Bridge 服务化部署 | ✅ | -webdir 静态托管 Web 后台（/api 优先、SPA fallback、no-store），API+管理页一个端口一个进程；scripts/build-release.sh（web build+本机二进制+可选 musl-cross 交叉+webdist 打包）；docs/DEPLOY.md（目标机构建/数据迁移须同 BRIDGE_MASTER_KEY/systemd 单元/防火墙/设备指向）。本机验证：/ 200 html、/xiaozhi SPA fallback、/api 正常、assets 200 |
| 订阅卡按类型渲染 + 实例排序 | ✅ | ① 设备端预热按钮原对每张卡无条件建且写死调智谱——改为聚合端点透出 `can_prewarm`（provider Capabilities，仅智谱 true），DeepSeek 卡不再出现；余额型卡（win_5h_s=0）隐藏环/周条/倒计时、中心改显余额大字（credits 首条，真网 DeepSeek ¥5.01）。② 智谱套餐/到期：`/api/biz/subscription/list` 真实响应是 data 数组 + productName + valid 区间串（旧解析按 data.list 失败吞成空 plan）——重写解析自适应双形态、排除 EXPIRED 等状态、PlanName="GLM Coding Lite · 12月11日到期" + 新增 `plan_expires_at` 结构化字段真网验证。③ 实例排序：subscriptions 表 ALTER 加 sort 列（ensureColumn 先例），新建 max+1、编辑不改序，新增 `POST /api/v1/subscriptions/order`（集合一致性校验）；Web 订阅页 ↑↓+保存排序（仿主页排布范式）+ 异步快照摘要行（套餐/到期/余额/错误）；删无调用 prewarmZhipu。④ 卡片重排（用户要求）：环 240 居中、环心=剩余百分比+重置倒计时、周额度收成中央单行+300 宽条、预热/刷新缩为 120×44 底部并排、状态行改 BOTTOM_MID 精简文案——所有控件按圆屏安全带（|dx|²+|dy|²≤240²）收敛，修掉原 BOTTOM_LEFT 被圆角切掉与右上卡越界。headless 双卡截帧断言通过（DeepSeek 环/周条像素为零）；check.sh 全绿；已刷机启动正常 |
| 订阅/天气自动刷新设置 + 订阅卡视觉对齐 | ✅ | ① 订阅：Bridge 新增 SettingKeyDeviceConfig + GET/PUT `/api/v1/device/config`（sub_refresh_interval_s 校验 300~3600 秒）；设备 remote config 两段式拉取下发（ds_device_config_fetch，mock 900）；**设备订阅页新增自动刷新 timer**——此前 `sub_refresh_interval_s` 设置页有控件但全仓无消费者（真 bug），到点 ds_refresh+数量变化重建；tile 刷新按钮补传 app_id（原 NULL 致数量变化时误开 app 0）。Web 订阅页加「设备订阅自动刷新」设置卡。② 天气：weather config 加 `auto_refresh_min`（指针区分未设置/显式关闭；0=关闭、5~1440 分钟），weatherSvc 改动态间隔（15s 检查配置、关闭=仅按需刷新，TTL 跟随间隔）；Web 天气页加「自动刷新」下拉（关闭/10/30/60/120）。③ 设备订阅卡按 gpt-image 设计稿细调：环 track 改灰紫 0x333A55、预热/刷新改胶囊圆角（radius 22）。真网/接口验证全过，已刷机启动正常 |
