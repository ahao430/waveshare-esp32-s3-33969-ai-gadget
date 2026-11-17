# AGENTS.md — AI 协作者工作手册

给在本仓库工作的 AI 编码代理（ZCode/Claude Code 等）的操作手册。
人类开发者同样适用。目标：不读完全部 PLAN.md 也能安全高效地干活。

## 项目一句话

Waveshare ESP32-S3 圆屏 AI Gadget 的完整自定义固件栈（**v1.0.0**）：
**模拟器先行开发 UI（LVGL/C）→ Go Bridge（凭据/额度/AI 路由/配置）→
Vue3 Web 后台 → ESP-IDF v6 固件**。四件套共享一份契约，模拟器与真机
共用 UI 源码（ADR-001，移植进行中）。

## 仓库地图（改哪里找哪里）

| 要改什么 | 位置 | 备注 |
|----------|------|------|
| 设备 UI（Launcher/App 页面） | `simulator/lvgl-pc/src/ui/` | **模拟器与真机共用源**；新增 .c 要手改 ui CMakeLists（两处） |
| UI 公共件（页点/手势） | `simulator/lvgl-pc/src/ui/ui_gesture.*` | 页点定位用 `ui_page_of_scroll`（实测宽+四舍五入，勿硬编码 480） |
| 全局状态栏 / App 后台指示 | `simulator/lvgl-pc/src/ui/ui_statusbar.*` | overlay 常驻 `lv_layer_top`；点右侧图标簇=快捷面板（音量+亮度，真机即时生效）；改动 `ds_config_persist()` 防抖回写 Bridge（亮度/音量跨重启持久化，开机 remote config 回放）；App 后台状态药丸用 `ui_statusbar_set_app_status()`（P1，ADR-012），不新增任务/栈 |
| UI worker 线程 | `simulator/lvgl-pc/src/ui/ui_worker.*` | 统一 `ui_worker_start()`：16KB 栈 ENOMEM 自动回落 12KB；新页面别再直接 pthread_create |
| UI 提示音 | `simulator/lvgl-pc/src/ui/ui_sound.*` | `ui_sound_click/alert()` 双平台封装；真机=audio_esp32 正弦合成短任务，模拟器=仅日志；LVGL 线程可直接调 |
| 数据源抽象 | `simulator/lvgl-pc/src/data_source.*` | mock / bridge 两实现；新字段三处同步（.h/两 .c） |
| TF 卡文件 | `simulator/lvgl-pc/src/data/data_source_files.c`、`sd_paths.h`、`ui/ui_files.c` | 单文件双平台（固件 /sdcard，模拟器 `GADGET_SD_DIR` 默认 `sdcard/`）；阻塞 IO 只能在 16KB worker 线程 |
| Bridge API | `bridge/internal/api/` | 标准库 net/http，Go 1.22+ 路由模式 `GET /api/v1/...` |
| Bridge 配置存储 | `bridge/internal/storage/` | bots/app_settings 表；密钥字段 Encrypt 后存 JSON |
| AI 上游（AstrBot/OpenAI 兼容） | `bridge/internal/ai/` | astrbot payload 契约见 astrbot.go 头注释；会话身份 per-bot（`gadget-<botID>`，「新对话」=轮换代，持久化于 app_settings.astrbot_sessions） |
| 语音链路 / 音色试听 | `bridge/internal/voice/` | ASR→LLM→TTS；`POST /api/v1/voice/preview` 只读试听（不落配置/不改冷却） |
| 小智官方服务 | `bridge/internal/xiaozhi/`、`docs/XIAOZHI_PROTOCOL.md` §5 | Bridge 模拟设备（OTA 激活 + WS + Opus）；**已实现并真网验证**（ADR-005）。bridge 的 go 命令要带 `-tags nolibopusfile` |
| Web 页面 | `web/src/pages/*.vue` | 加页要同步 router/index.ts + App.vue nav + Dashboard 入口 |
| 固件 | `firmware/main/`（bring-up 骨架）、`firmware/components/power_manager/`、`firmware/components/wifi_prov/`（运行期配网：NVS+扫描+试连；Bridge 地址簿+预置） | UI 移植阶段会把 simulator 源编进来（main/CMakeLists 预留注释） |
| 中文字体 | `scripts/gen_cn_font.sh` → `simulator/lvgl-pc/src/ui/fonts/font_cn_*.c`；番茄钟加粗字体 `scripts/gen_pomo_font.sh`（Noto Sans SC wght=700，20px） | 改字典/加字后必须重新生成，否则真机/模拟器豆腐块；番茄钟文案改动也要重跑 gen_pomo_font.sh。**勿为单页新增独立字库**——rodata 与 PSRAM 堆共享映射窗口，见坑 #25 |
| 大号数字 | `scripts/gen_cn_font.sh` 末尾生成 `font_digits_84.c`（Montserrat 84px 子集） | 番茄钟倒计时用；逐位固定宽度渲染防抖 |
| 桌面图标 | `simulator/lvgl-pc/assets/icons/`（88×88） | 缺位自动回退符号；名字与 launcher catalog key 对齐；`gen_icon_embed.sh` 的 SKIP 里的图标不嵌入固件（省 flash） |

## 提交前必跑

```bash
scripts/check.sh   # bridge: go vet+build+test / web: typecheck / simulator: cmake / firmware: idf.py build
scripts/regress-sim.sh   # UI 改动建议加跑：8 App headless 截帧回归（PASS 基线 8/8 唯一画面）
```

全绿才提交。只改 docs 可以豁免。

**改了 `web/` 必须同步部署产物再提交**（2026-11-17 起 `bridge/webdist/`
随仓库分发——NAS/他人 git pull 即得后台页面，无需 node）：

```bash
cd web && npm run build && cd .. && rsync -a --delete web/dist/ bridge/webdist/
```

注意 `.gitignore` 只忽略 `web/dist/`（构建中间产物），
`bridge/webdist/` 是有意入库的（`-webdir` 缺省探测目录）。

## 固件环境（本机现状）

- ESP-IDF **v6.0.3** at `~/esp/esp-idf-v6.0.3`，venv `~/esp/idf-buildenv`
- 激活：`export PATH="$HOME/esp/idf-buildenv/bin:/opt/homebrew/bin:$PATH"; . "$HOME/esp/esp-idf-v6.0.3/export.sh"`
- 设备串口：`/dev/cu.usbmodem1101`（USB-Serial/JTAG）
- **刷写前必杀 monitor**：`pkill -9 -f "esp-idf-monitor"`，否则端口占用刷写失败
- Wi-Fi 凭据：`firmware/main/wifi_secret.h`（gitignore；模板 .example）
- **不备份不刷写**：原厂固件唯一备份在 `~/gadget-factory-backup-20261114.bin`

## 硬约束（违反=返工/真机事故）

### 安全
- API Key/Token 只进 Bridge（AES-256-GCM），不进固件、日志、Git、二维码
- Bridge 响应永不回显密钥明文；日志不记 Authorization/查询串

### UI
- UI 风格以 `assets/ui/concepts/assistant-ui-final.png` 为准（定稿，勿自由发挥）
- 桌面/对话页布局与配色见 docs/UI_DESIGN.md

### 真机特有坑（均已实测，HARDWARE.md §2.7 有完整记录）
1. **`bsp_display_lock(0)` 实际是 0-tick try-lock**（头文件写"无限等待"，
   实现转 `esp_lv_adapter_lock(0)`，与文档不符）。失败被忽略就会并发踩
   LVGL → 断言 `Invalidate area is not allowed during rendering` →
   `while(1)` 静默死机（屏幕半帧、IDLE0 WDT）。**一律用
   `esp_lv_adapter_lock(-1)`**（`ui_lock/ui_unlock` 已封装）+ 成对 unlock；
   背光要等 `ui_init()` 之后才开（否则与首帧 DMA 抢 QSPI 总线）
2. **全屏高频重绘会堵死显示管线**（白屏/LVGL 挂起，断电才能恢复）——
   设备 UI 只做局部失效 + 帧率节流；全屏刷新仅限页面切换
3. `esp_timer` 周期单位是 **µs**（按 ms 传会快 1000 倍）
4. KEY3（GPIO18）机械抖动需 200ms 去抖
5. 新增头文件后 ninja 依赖不回溯——`touch main.c` 强制重编
6. GOT_IP 等事件回调里更新 UI：必须持显示锁；Wi-Fi 启动要在 UI 构建之后

### 2026-11 稳定性踩坑（真机，勿回退）

1. **绘制缓冲固定内部 RAM 10 行双缓冲**（`use_psram=false`，
   `max_transfer_sz=480*10*2`）。放 PSRAM 时 SPI 每块要在内部 RAM 临时
   分配同尺寸 bounce buffer，聊天页内存碎片化后必失败 →
   `Draw bitmap failed: NO_MEM` → 花屏 + flush 卡死 + 看门狗重启。
2. **语音回合用常驻静态栈任务**（`xTaskCreateStatic` + `.bss` 14KB）。
   pthread 动态栈在内部 RAM 碎片化时 ENOMEM（rc=12，状态卡"思考中"）；
   栈放 PSRAM 会 `InstrFetchProhibited`。
3. **LVGL 心跳看门狗**：判据是 `g_lvgl_beat` 连续 15s 不推进，不是
   「锁 BUSY 连续 3 次」——聊天页重绘慢时会误杀。hangprobe 栈 8KB
   （4KB 实测溢出）。TWDT 10s 负责真楔死自愈（约 20s，rst:0xc）。
4. **选机器人面板**：拉列表 worker 栈 ≥16KB（12KB 实测溢出），UI 构建用
   `lv_async_call` 回 LVGL 线程。
5. `ui_launcher_rebuild` 前必须清页点数组（悬垂指针 LoadProhibited）；
   波浪背景 buffer 要复用，否则重建泄漏。
6. 远端配置两段式：`ui_remote_config_prefetch`（不持锁做网络/文件）→
   `ui_remote_config_refresh`（持锁应用 + `app_manager_clear` + 重建桌面）；
   GOT_IP 后必须补拉，否则开机排布/主题不生效。
7. **字体**：lv_font_conv 默认 RLE 压缩，必须开 `LV_USE_FONT_COMPRESSED`
   （固件 Kconfig + 模拟器 lv_conf.h），否则空心方块；生成 struct 自带的
   `.fallback = NULL` 会覆盖挂接；GBK 区间会混入 7000+ 繁体导致 flash 溢出。
   当前**不压缩、无 L2 回退**（用户决策），app 分区 12M 余 68%（ADR-013
   重分区后），加功能不再需要裁字体/素材（分区表见 partitions.csv，
   OTA 双槽已砍）。
8. ~~启动期 50% 概率显示管线楔死~~（2026-11-17 降级：**已消失，疑似
   被坑 #11 顺带治好**）——坑 #11（LVGL 线程禁一切阻塞调用）落地后
   连续 6+ 次开机零复发（原 50% 概率下概率仅 ~1.6%）；启动期楔死与
   §2.10「公司网冻屏」同根因（启动期状态栏/倒计时首个 tick 即撞
   ds_state() 惰性刷新 → Bridge 不可达时 lwip 阻塞）。TWDT 自愈机制
   保留作兜底；若复发按 §2.10 的取证方法抓 lvgl 任务栈。
9. **ES7210 采集任务必须常驻**（`audio_esp32.c`，别按轮启停）：SPEAKING
   期间无人读 mic → RX DMA 溢出后 `esp_codec_dev_read` 永远返回零数据
   （rc=0 无报错），表现为"下一轮起转写永远是同一句"。现在 capture
   任务常驻，start/stop 只切换消费方。
10. **主任务发 QSPI polling 事务必须持 display_lock**：开机背光
    `tx_param` 与 LVGL 首帧 DMA 并发会撑死 `spi_device_release_bus`
    断言（实测撑 125s 后崩溃重启）。`bsp_display_backlight_on` 已持锁。
11. **LVGL 线程内禁一切阻塞调用（2026-11-16 冻屏根因，HARDWARE §2.10）**：
    `ds_state()` 曾带"首次访问惰性刷新"，Bridge 不可达时被状态栏(5s)/
    倒计时(1s)在 LVGL 线程里反复同步发 HTTP → 持显示锁冻屏（"家里正常、
    公司必现"）。规则：`ds_state()` 只读缓存；页面/按钮的网络刷新一律
    worker 线程 + `lv_async_call` 回 UI（页面代际计数防悬垂控件）；
    `http_client` connect 带 5s 上限；探针/net_probe 不占看门狗任务。
12. **Wi-Fi 扫描别先断开再扫（2026-11-16）**：连接态直接
    `esp_wifi_scan_start` 即可（实测 46~63 APs，不打断连接）；只在返回
    `ESP_ERR_WIFI_STATE`（正在连接）时才断开中止、**等 DISCONNECTED 事件
    落定**再扫。曾用固定 150ms：从已连接态 deauth 要 300ms+，扫描被驱动
    静默吞掉（返回 OK 但 0 结果）——真机"配网页列表永远空"。
13. **16KB pthread 栈在真机可能 ENOMEM（2026-11-16）**：内部 RAM
    碎片化后 largest free block 只剩 ~8.7KB，`pthread_create` 申请
    16KB 栈返回 **rc=12**，worker 静默不启动——订阅页取数 worker 因此
    永远不跑，页面一直空（天气/文件 worker 同款风险）。统一用
    `ui_worker_start(fn, arg, size)`：ENOMEM 时自动回落 12KB 重试
    （实测订阅 HTTP 链只用了 ~3.7KB 栈，余 8.5KB）。新页面的重复
    worker 不要直接 `pthread_create`。
14. **SPIRAM_FETCH_INSTRUCTIONS / SPIRAM_RODATA 与 LVGL 栈位置是
    成对约束，不许单独改（2026-11-17 深夜连环崩溃实证）**：把这两个
    开关关掉（代码/常量回 flash）当晚即出现三类崩溃：①companion
    动画读 assets 分区（LVGL 线程 esp_partition_read）时触摸中断打进
    来 → **Cache error**；②LVGL PSRAM 栈任务做 flash 操作 → assert
    `esp_task_stack_is_sane_cache_disabled`（cache_utils.c 只认 DRAM
    栈）；③内核数据（xSuspendedTaskList/SPI 总线锁）被写坏 → 渲染
    线程 __stack_chk_fail。全部回滚（FETCH/RODATA=y +
    stack_in_psram=true，2026-11-16 下午组合）后稳定。**守则：这两个
    开关与 `stack_in_psram` 必须维持 f55e6a3 原值成对存在，改任何一个
    都要先在真机跑满一晚再合入。**另一面：=n 时 PSRAM 堆池会从 4416K
    涨到 8192K（直连版实测对照，同一块板），=y 的稳定是以 3.7MB PSRAM
    堆为代价换来的——两头都不便宜，当前取稳定。**
15. **对话页低频后台操作必须投静态任务，不能 pthread（2026-11-17）**：
    "选机器人"面板拉列表 pthread 16KB 在对话页稳态 largest_int
    ~8.7KB 时 ENOMEM 静默失败——点击无任何反应，用户以为按钮坏了。
    picker/sync/apply/reset 四个 worker 已改为投递给常驻静态栈语音
    任务（s_botw_kind 分发，与 PTT 互斥）。新加低频后台操作照此模式。
16. **esp_audio_codec 2.x 必须显式注册编解码器（2026-11-17）**：走
    统一 API（esp_audio_enc_open/dec_open）前要先调
    `esp_opus_enc_register()` / `esp_opus_dec_register()`，否则
    `AUDIO_DEC: Decoder OPUS(1398100047) not registered`，open 静默
    返回 NULL。（直连尝试期间发现，xz_opus.c 有现成实现）
17. **esp_websocket_client 不适合本板 WSS（2026-11-17）**：其任务栈
    写死 4KB 且走内部 RAM——对话页稳态建不出来
    (`Error create websocket task`)，建出来 TLS 握手也必溢出（TLS 链
    实测要 16KB 栈）。若重启小智直连，用 esp_tls + RFC6455 自实现
    同步客户端跑在静态任务里（xz_session.c 有完整实现可复用）。
18. **mbedtls 4.x 错误码 -0x008D = PSA_ERROR_INSUFFICIENT_MEMORY
    （=-141），不是加密层损坏（2026-11-17 两次误诊修正）**：OTA TLS
    在内部 RAM 紧张时全部内存分配失败报这个码。解法（已验证）：
    `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`（TLS 缓冲走 PSRAM，官方
    xiaozhi-esp32 S3 同款）+ `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`
    （默认 16384 的 in_buf 在对话页必 OOM）。此配置下 OTA 已真网跑通
    （bound=1）。
20. **LVGL 控件内存默认吃内部 RAM——配网面板曾把 internal 吃到 328B
    （2026-11-17）**：`LV_USE_CLIB_MALLOC` 下控件走 libc malloc，若
    `SPIRAM_MALLOC_ALWAYSINTERNAL` 设大（曾 16384）则几百个小对象全
    挤内部 RAM → `spicommon_dma_setup_priv_buffer` 失败 → Draw bitmap
    NO_MEM 刷屏（屏幕无输出但 UI 逻辑正常）。**定值 2048（官方
    xiaozhi-esp32 S3 同款）**：小分配去 PSRAM，内部 RAM 留给
    Wi-Fi/DMA/驱动。配网列表/键盘是重灾区。
21. **SNTP 只能初始化一次（2026-11-17 配网重连 abort 实证）**：
    `net_task` 事件循环每次 GOT_IP（初连/重连/切网）都跑
    `esp_sntp_setoperatingmode` → lwIP assert "Operating mode must not
    be set while SNTP client is running" → abort 重启。任何"每次网络
    事件都执行"的初始化代码都要查幂等性（SNTP/事件组/timer create）。
22. **大块 PSRAM 连续分配不可靠——按最大回复定缓冲（2026-11-17）**：
    TTS 缓冲 1.5MB 单块分配在 LVGL 小对象搬入 PSRAM 后失败
    （'tts buffer alloc failed' 刷屏 → 有字幕无声音）。768KB
    （24s@16k）实测稳，覆盖实际回复 272KB/16.8s。**PSRAM 大块
    分配上限经验值 ~768KB**（碎片化后 1MB+ 不可靠）。
24. **智谱 Coding Plan 套餐端点：`/api/coding/paas/v4`（2026-11-17
    三重实证）**：预热要消耗套餐额度必须走**专用端点**
    `https://open.bigmodel.cn/api/coding/paas/v4/chat/completions`
    （来源：workspace/AIVault 的 kZhipuOpenaiBase 常量，社区验证）。
    普通端点 `/api/paas/v4`：套餐 key 调套餐模型（glm-5.3）直接
    **429**；调免费模型（glm-4-flash）→ 200 但不计套餐、不启动 5h
    窗口（before/after 均 0%/100%）。coding 端点实测：glm-5.3 + "hi"
    + max_tokens=1 → **5h 额度 100%→99%、reset_at 从 None 变为
    5 小时后**。注意：官方文档"仅官方工具内使用"指的是工具内部
    走的就是这个端点——自建程序显式指定 coding 端点等效。
    设备端预热按钮已恢复真实请求（确认弹窗注明消耗 ≈1%）。
23. **公司访客网可达性因目标而异（2026-11-17 修正）**：GYJX-GUEST
    对旧 VPS 域名（wavesharebridge.ahao430.site，80）TCP 超时，但
    对 NAS Bridge（route.ahao430.site:8081）完全正常（语音 40KB
    上传）。"访客网阻断"不能一概而论——设备地址簿多地址备案、
    网络诊断逐地址探测才是正解。
19. **Wi-Fi 驱动初始化有内部 RAM 底线（2026-11-17 开机 abort 循环实证）**：
    post-ui 内部 free <~56KB 时 `esp_wifi_init` 静态 RX 缓冲分配不足
    （`Expected to init 10 rx buffer, actual is 7/8`）→ ESP_ERROR_CHECK
    abort → 开机无限重启。改任何静态 .bss/保留额度前先算这本账：
    RESERVE_INTERNAL 或 .bss 多吃 ~12KB 就够把 Wi-Fi 顶下线。

25. **rodata 与 PSRAM 堆池共享 8MB 映射窗口——单页独立字库直接挤垮语音（2026-11-17）**：
    为订阅页加粗做了三档 GB2312 全集字体（font_sub_12/14/16，+1.67MB rodata），刷机后
    PSRAM 显示 86%（堆池 4416K→~3MB），TTS 的 768KB 大块分配失败，三个聊天语音全哑。
    **规则：`SPIRAM_RODATA=y`（坑 #14 组合）下 .rodata 每涨 ~1MB，PSRAM 堆池就缩 ~1MB；
    加字库/内嵌素材前先算 rodata 池账（idf.py size 看 .rodata），单页美化需求一律优先
    用现有 font_cn_*/font_pomo_* 解决。**已整体回退（池恢复 4416K）。

### 内存 / flash 账本（2026-11-17 实测，Bridge 链稳定版）

> 数字全部来自真机串口日志与 `idf.py size`，改内存相关配置前后对照这本账。

**flash（16MB）**：
| 区 | 大小 | 说明 |
|----|------|------|
| bootloader | 21.2KB / 32KB | |
| nvs | 88KB | 配网/凭据/小智身份（ADR-013 扩容自 24KB） |
| factory(app) | **5.57MiB 已用 / 11.94MiB（47%，余 53%）** | 2026-11-17：GB2312 全集字库并入主库（净 +2.0MB，删 dyn_14）。订阅页加粗字库三档（+1.67MiB）当日回退——挤占 PSRAM 堆池致聊天语音哑（坑 #25） |
| assets | ≤3584KB | 伴宠动画资源包（gen_companion_bin.sh） |
| storage | 448KB | FATFS（TF 卡不存在时的兜底分区） |

**DIRAM（334KB 总量）**：静态已占 235.6KB（69%，.bss 104KB + IRAM .text 102KB + .data 23.6KB）。
**内部 heap 实测水位（关键路径）**：
| 阶段 | internal free / 最大块 |
|------|----------------------|
| 开机显示前 | 143KB / 77.8KB |
| 显示就绪 | 90.9KB / 31.7KB |
| 桌面就绪（post-ui） | **70KB / 31.7KB（Wi-Fi init 前提，坑 #19）** |
| 桌面稳态（tick） | **16.1KB / 8.7KB** |
| 对话页打开 | 14.9KB / 7.7KB |
- .bss 大项：语音任务静态栈 16KB（16384，picker/sync/apply/reset 共用）、绘制缓冲 19.2KB（480×10×2 双缓冲）、hangprobe 8KB。
- **改任何 .bss/保留额度前先算 Wi-Fi 底线**：post-ui free 掉到 ~56KB 以下 `esp_wifi_init` 分不到 10 个 RX 缓冲 → 开机 abort 循环（坑 #19）。

**PSRAM（8MB octal @80MHz）**：**进堆的只有 ~4.4MB**（4416K+23K 池；**rodata 涨堆池就缩**，font_sub 事故实证，坑 #25，
  `Adding pool of 4416K`）——FETCH/RODATA=y 的虚拟地址窗口划分代价
  （这两开关 =n 时池可到 8192K，直连版实测，但会引爆坑 #14 崩溃，见下）。
  2026-11-17 坑 #25 修复后真机实测（系统状态页口径）：开机桌面已用
  ~30%（余 ~3.0MB）；语音回合后 ~73%——TTS 768KB（AUDIO_TTS_MAX_BYTES，
  坑 #22 从 1.5MB 降档，分配后常驻）+ 录音缓冲 256KB（G_CAPTURE，
  8s@16k）+ 回合临时分配，余 ~1.2MB。**给 PSRAM 大块分配留的余量
  上限就是 ~1.2MB**。

**Mac 侧服务**：Bridge 进程 RSS ~15MB（Go，API+Web 静态托管同进程同端口 :8081）；无独立 Web 进程。

**App 内存 delta 测量法**：固件 app_manager 在 App open 前后打 `appmem` 日志（heap/PSRAM/largest_int 前后值），看实测 delta 别估算。已实测：chat 打开 = PSRAM -82.8KB + 内部最大块 15.3→7.7KB；其余 App 待测，测法同。

### 当前能力边界（对外说明别说过头）

- **小智 App 已接官方 xiaozhi.me**（ADR-005，2026-11-16 真网验证）：
  Web「小智」页连接官方服务（6 位激活码在官方控制台绑定）后，设备小智
  App 的转写/回复/音色全走官方，人设音色在官方控制台配；未绑定或官方
  失败自动回落本机 ASR→LLM→TTS（语音设置页的音色在回落时生效）。
  一个 Bridge 身份 = 官方控制台里的一台设备。
  设备**直连官方**的尝试（ADR-014）已跑通 OTA 激活与 WSS 握手，但与
  本板内存约束冲突未收敛，已**暂停接线回滚 Bridge 链**（ADR-015）；
  源码存档 firmware/main/xz_*.c/h（未编译），重启项目见
  docs/XIAOZHI_DIRECT.md 的「证据索引」。
- **运行期配网已实现**（ADR-009）：设备「设置→网络与连接」可扫 Wi-Fi、
  软键盘输密码（存 NVS，NVS 优先于 `wifi_secret.h` 编译兜底）、热改
  Bridge 地址（`ds_bridge_rebind` 立即生效）。`wifi_secret.h` 只在
  NVS 从未配置过时生效（启动日志 `creds=` 可辨认来源）。
- **TF 卡（ADR-010/011）**：`/sdcard` FATFS 挂载后用于配网备份/离线缓存/
  文件管理；目录契约见 `sd_paths.h`（backup/cache/data/appearance/
  downloads，统一经 `sd_root/sd_resolve` 访问）。未插卡全部功能静默降级。
  备份含 Wi-Fi 明文密码（勿外借）。上传/下载单文件 ≤2MB（1-bit SDMMC
  吞吐未实测，见 HARDWARE §2.5）。
- 设备只连 Bridge（默认 `http://<主机>:8081`），不连 Web。Bridge 可
  部署在任意常驻主机（`-webdir` 托管 Web 后台，部署链见
  `docs/DEPLOY.md`）；开发期在 Mac 上跑则 Mac 睡眠/关机对话失败。
- **设备 HTTP 只支持明文、不带鉴权**：`http_client.c` 只认 `http://`
  （无 TLS），也不发 `Authorization`。公网 Bridge 必须开 80/8081 明文端口
  （https-only / http→https 强跳设备连不上），且**不能设 `BRIDGE_TOKEN`**
  （设备全部 401）。无鉴权暴露面用非默认端口 + 防火墙白名单收窄（DEPLOY §5）。
  地址簿可用 `wifi_secret.h` 的 `BRIDGE_SEED_HOSTS` 开机预置（幂等，不改当前）。
- **局域网后台**：Bridge 监听 `:8081` 全部网卡，Vite 已 `host: true`，
  手机访问 `http://<MacIP>:5173`。仅限可信局域网；设了 `BRIDGE_TOKEN` 时
  浏览器需 `localStorage.setItem('bridge_token', ...)`（无登录页）——但
  设备链路不支持 token，公网部署两者不可兼得（见上一条）。
- **订阅实例 = 唯一配置入口**（Web 订阅中心）：实例自带 Key，保存时
  Bridge 把 Key 投影到凭据表（语音 ASR `resolveAccount` 动态解析同一份）；
  独立凭据编辑区已删（2026-11-16 用户拍板），别恢复两层概念。设备订阅页
  只读订阅实例（subscriptions 表），凭据表仅是 Key 的投影+语音侧存储。
- **Muse 授权必须先刷官方 SDK**：本项目固件不能发起 Muse 配对。流程是
  刷官方 Muse SDK 固件 → 手机 Muse App 配对授权 → `esptool read-flash
  0x11000 0xC000` 导出 NVS 凭据 → 刷回本项目固件 + 重烧伴宠 assets →
  Web Muse 页填凭据。官方固件会覆盖伴宠资源分区。
- **USB 线**：只负责供电/串口日志；接了电池可拔线独立运行，没接电池拔线即断电。

### 物理键固定行为（不可配置，2026-11-17）

| 键 | 短按 | 长按 |
|----|------|------|
| 电源 PWR（AXP2101 PWRON） | **息屏/点亮切换**（屏保态：黑底时钟+15% 亮度；触摸/KEY3 也可唤醒） | **关机**（PMU 硬关 10s 兜底 + 优雅关机） |
| BOOT | 返回 | 配网页（固定） |
| KEY3 | 交给应用（可配） | 语音说话 PTT（可配） |

- 电源键两态行为在固件硬编码（main.c PWR CLICK/LONG 分支），Web 按键页只读展示；Bridge keymap 的 `pwr_short` 字段仅为 API 兼容保留——已移出可配置槽位（POST 一律忽略、恒返回存储值），设备与模拟器均不消费（模拟器 ui_keys.c PWR 分支为无操作）。
- 改这两个行为 = 改固件（用户 2026-11-17 明确要求锁定）。

### 后台运行规则（ADR-011，新增 App 必须遵守）

| App | 退出前台后 |
|-----|-----------|
| AstrBot / 小智 / Muse | on_exit 停 worker/音频，保存会话状态 |
| 订阅 / 天气 | 停自动刷新 timer，保留 TF 缓存 |
| 文件 | 关闭句柄，worker 结果按页面代际丢弃 |
| 设置 | 保存配置，释放页面 |
| 番茄钟 | **lv_timer 常驻后台计时**（on_exit 只清 UI 引用，不删 timer）；阶段归零三连滴提示音（后台也响，`ui_sound_alert`） |

- App 级后台任务（如番茄钟）的 timer 不能随屏幕销毁；回调必须用
  `g_root==NULL` 守卫跳过所有 LVGL 调用（屏幕已被 app_manager 删除）。
- App manifest 用 `APP_FLAG_BG_TIMER/STATUSBAR/KEEP_STATE` 声明后台
  语义（番茄钟 = `BG_TIMER|STATUSBAR`）；`appmem` 日志带 `flags=0x..`。
- **重复** UI timer 用 `app_manager_timer_create()` 创建，切走 App 时
  由 app_manager 统一删除（`on_exit` 只断引用）；一次性 timer 会被
  LVGL 自动删除，**不要登记**；后台常驻 timer（番茄钟）也不要登记。
- 新增 App 默认"退出即停"；要做后台任务先补本表 + ADR。
- **内存观测**：固件 `app_manager` 在 open/back 打 `appmem` 日志
  （flags/heap/min/PSRAM free/内部 RAM 最大连续块），评估新 App 成本
  看实测 delta，不靠估算；模拟器无 ESP heap，不打印。
- TF 应用数据写 `data/`（如 `data/pomodoro.json`），缓存写 `cache/`；
  都要 worker 线程做 IO（踩坑 #11），统计类文件用 temp+rename 原子替换。

### LVGL 版本差异（移植期主要摩擦）
- 模拟器 LVGL 9.2.2 / 固件更新：`lv_obj_add_flag/clear_flag` 在固件是
  deprecated-error，改用 `lv_obj_set_hidden/clickable` 等专用 setter
- 触摸单点（无手势库），交互按单点设计

## 验证方式（按信任级）

1. `scripts/check.sh` 全绿——编译级
2. Bridge：`go test ./...`（api/storage 层有契约测试）
3. 模拟器像素验证：`GADGET_SNAP=/path.ppm` 截帧 + PPM 直读/差分脚本
   （页点位置、颜色值、标签 span 都可断言）；headless 钩子
   `GADGET_DEMO_APP=<枚举号>`（ui.h enum 序号：0=智谱 1=对话 2=Muse
   3=小智 5=文件 6=设置 7=天气 8=番茄钟） / `GADGET_DEMO_KEYS="..."` /
   `GADGET_DEMO_VOICE=1`（2026-11-17 勘误：AGENTS 曾写 DEMO_APP=<key>
   字符串，代码实读 GADGET_DEMO_APP 数字枚举）
4. 真机：idf.py monitor 收日志 + 用户目视验收（屏幕/声音/触摸是主观项，别替用户确认）

## 文档同步纪律

改了什么就回写哪里：
- 硬件事实/坑 → `docs/HARDWARE.md`（§2.6 地址表 / §2.7 首验与坑）
- bring-up 清单 → `docs/BRINGUP.md`（勾选 + 实测注记，**没验证的不勾**）
- 阶段成果 → `docs/PROGRESS.md`
- 重大取舍 → `docs/DECISIONS.md`（ADR）
- 产品需求变更 → `PLAN.md` 对应节（如 §13.4 充电屏保）

## 提交规范

- 中文提交信息，前缀标域：`firmware:` `bridge:` `web:` `simulator:` `docs:` `repo:`
- 一批工作一个提交（bring-up 的一轮=一个提交），信息里写**真机实测证据**（数值/日志结论）
- 不 amend 已推送提交；凭据文件永不入库（wifi_secret.h 已 gitignore）

## 服务端口（开发机常驻）

- Bridge `:8081`（监听全部网卡；master key 在 `bridge/bridge.env`，日志 `/tmp/bridge.log`；
  重启必须带同一个 `BRIDGE_MASTER_KEY`，否则加密凭据读不出来；
  `-webdir <dir>` 可托管 Web 后台静态文件（缺省自动探测 webdist/ 或 ../web/dist）——API+管理页同端口）
- Web `:5173`（Vite `host: true`，局域网 `http://<MacIP>:5173`；`/api` 服务端代理到 127.0.0.1:8081）
- 模拟器 GUI 可常开（bridge 模式实时连）
