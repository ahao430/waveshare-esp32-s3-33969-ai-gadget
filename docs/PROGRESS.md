# 进度对照表（PLAN §18 验收标准逐项核对）

> 最后更新：2026-11-14。状态标记：✅ 完成 · 🔶 部分完成 · ⏳ 未开始 ·
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
