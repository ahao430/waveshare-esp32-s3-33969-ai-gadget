# 开发环境与验证记录

按 PLAN §21 要求，记录每次构建/测试的命令、结果与遗留问题。
最近一次全量验证：2026-11-14。

## 环境

| 工具 | 版本 | 状态 |
|------|------|------|
| macOS | 25.5 (arm64) | ✅ |
| Go | 1.25.6 | ✅ |
| Node / npm | 18.15.0 / 9.5.0 | ✅ |
| cmake | 3.31.6 | ✅ |
| SDL2 | brew `sdl2-compat 2.32.70`（pkg-config 可用） | ✅ |
| ESP-IDF | **未安装** | ❌ 固件本机构建 TODO |

## 构建与测试记录

### Bridge（Go）— ✅ 通过

```bash
cd bridge && go vet ./... && go build ./... && go test ./...
# ok  gadget-bridge/internal/quota/zhipu 0.013s
```

- 智谱 Adapter：fixture 单测 5 组（窗口映射/已用-剩余分离/毫秒时间戳/
  套餐失败不影响额度/未知结构报 unsupported_response）全部通过。
- API 冒烟：`/api/v1/quota/providers`、`/api/v1/quota/mock/snapshot`、
  未知 provider 404 语义正确。

### LVGL PC 模拟器 — ✅ 构建/运行通过

```bash
cmake -S simulator/lvgl-pc -B simulator/lvgl-pc/build
cmake --build simulator/lvgl-pc/build -j
./simulator/lvgl-pc/build/gadget_sim   # 运行 4s 存活，无崩溃
```

- LVGL v9.2.2（FetchContent 拉取）+ 内建 SDL 驱动。
- 踩坑记录：`lv_conf.h` 需要
  `target_include_directories(lvgl PUBLIC <sim目录>)`，
  仅定义 `LV_CONF_INCLUDE_SIMPLE` 不够（lvgl 自身源码也要能找到它）。
- 未做：像素级目视验收（需要人工运行窗口确认布局）。

### Web（Vue3+TS+Vite）— ✅ 通过

```bash
cd web && npm install && npm run build
# vue-tsc --noEmit && vite build → ✓ built in 608ms
```

- 端到端链路验证：vite dev `/api` 代理 → Bridge `:8081` → Mock provider
  返回 JSON（curl 实测通过）。
- 踩坑记录：vite 默认只绑 `::1`，`curl 127.0.0.1:<port>` 连不上；
  联调用 `--host 127.0.0.1`。

### Firmware（ESP-IDF）— ✅ 首建通过（2026-11-14，v6.0.3）

环境（本机专用，换机需重做）：

- ESP-IDF **v6.0.3** shallow clone 至 `~/esp/esp-idf-v6.0.3`
  （git ls-remote 选的最新 v6.0.x，恰为 Muse 要求的精确版本）。
- **python 坑**：系统 python3.10（python.org 版）证书损坏导致
  install.sh 失败；用 `PATH=/opt/homebrew/bin:$PATH ./install.sh esp32s3`
  （brew python）装好 `idf6.0_py3.14_env`。
- **cmake 坑**：IDF 6.0.3 macOS 不自带 cmake，brew cmake 3.31 对 IDF
  生成的 temp 文件报参数语法告警并连锁失败（[IDFGH-12134] 相关）；
  用 `~/esp/idf-buildenv`（python3.14 venv + pip 的 cmake 3.30.5 + ninja）
  前置到 PATH 解决。注意 venv 的 python 次版本必须与 IDF python env
  一致（3.14），否则 export.sh 找不到对应环境。
- **lvgl 下载坑**：`lvgl/lvgl 9.6.0~1` 组件 115MB，组件管理器下载
  卡死在 CDN 重定向（components-file.espressif.com → .com.cn；
  curl -L 实测 9 秒 12.5MB/s）。解法 = `scripts/fetch_lvgl_component.sh`：
  手动下载解压到 `managed_components/lvgl__lvgl/`，并用组件管理器自带
  `idf_component_tools.hash_tools` 生成 `CHECKSUMS.json` +
  `.component_hash`（缺了会报完整性错误）。`override_path` 本地覆盖
  不可行：`esp_lvgl_adapter` 硬编码组件名 `lvgl__lvgl`。
- **flash 大小**：sdkconfig.defaults 需显式
  `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`（默认 2MB，15.1MB 分区表放不下）。

构建命令（完整脚本见 `/tmp/idf_build.sh` 的逻辑，要点）：

```bash
export PATH="$HOME/esp/idf-buildenv/bin:/opt/homebrew/bin:$PATH"
. ~/esp/esp-idf-v6.0.3/export.sh
cd firmware && idf.py set-target esp32s3 && idf.py build
# → Project build complete. ai_gadget.bin ≈ 0.6MB
```

尚未烧录真机（设备未到手）；`idf.py -p PORT flash monitor` 待 Phase 6。

[IDFGH-12134]: https://github.com/espressif/esp-idf/issues/13190

## Phase 5 补充（2026-11-14）：小智协议研究 + 双引擎演示

- **docs/XIAOZHI_PROTOCOL.md**：上游 WS 协议全量梳理（鉴权头 4 件套/
  hello 双向握手/二进制 Opus 帧 v1-v3/stt-llm-tts 消息/**IoT=MCP
  JSON-RPC**/状态机与重连）；33969 移植双方案对比（组件集成 vs 上游
  加板），**结论 = 方案 A 组件集成**（协议层组件化 + voice_session
  租约接入），工作量集中点 = QSPI AMOLED 的 Display 适配（上游无现成
  类）与 codec_dev 桥接；`apps/xiaozhi/` 客户端骨架设计稿（4 模块 +
  状态机映射表）。
- 模拟器语音演示页参数化：Muse 位与 Xiaozhi 位共用 `ui_voice_build`
  （engine_id = `muse-demo`/`xiaozhi-demo`），桌面两个语音入口都能
  走完整会话流；ADR-003 的租约互斥由单前台模型天然保证。
- 聊天 provider 联动：`GADGET_CHAT_PROVIDER=zhipu` 环境变量切换聊天
  模型路由（拼接 `<provider>/dummy`）；TODO 设置页选择。

## Phase 6 准备（2026-11-14）：真机 Bring-up 材料

- **骨架固件加入 I2C 全地址扫描**：上电先扫 0x08–0x77 并打印 ACK 地址 +
  期望芯片名（对照 HARDWARE.md §2.6 待验证清单）。地址事实的最终裁定，
  扫描结果回写 HARDWARE.md。编译通过（`bsp_i2c_get_handle` 是返回值
  而非出参——BSP 头文件核对过）。
- **docs/BRINGUP.md**：Phase 6 执行手册——开箱安全（先不接电池）→
  首连/烧录 → 启动日志三批证据 → 逐驱动验证清单（显示/触摸/音频/
  TF/PMU/RTC/IMU/按键/睡眠/Wi-Fi，每项带预期现象+失败排查）→
  性能功耗记录模板（PLAN §14.3）→ **Muse 板级坑 checklist 6 条
  逐条复核表** → 偏差记录表（发现即回写 HARDWARE.md）。
- 设备到手即可按手册开工；骨架固件 + 扫描能力已就绪。

## Phase 3 收官（2026-11-14）：凭据加密存储 + 账号配置 UI

### internal/security（AES-256-GCM，标准库零依赖）

- 密文格式 `v1:<b64 nonce>:<b64 ct>`（版本前缀支持将来密钥轮换）；
  GCM 认证保证篡改/错钥解密必败（单测含篡改翻转 + 错钥 + nonce 随机性）。
- 主密钥 `BRIDGE_MASTER_KEY`（64 hex chars = 32 字节）；**缺失 =
  凭据持久化整体禁用**（fail-safe；env Key 仍可用）。
  部署引导：`security.GenerateMasterKey()`。

### storage.provider_credentials

- CRUD：SaveCredential（加密 upsert）/ GetCredential（解密）/ Delete
  （幂等）/ ListCredentials（无明文视图）。masterKey=nil 全部拒绝。
- e2e 实测：**SQLite 文件 grep 不到明文 Key**；响应/API 永不回显。

### main 改造：zhipu/kimi 恒注册 + CredentialResolver

- provider 不再依赖 env Key 存在才注册——恒注册，账号经 resolver
  动态解析（**env 优先 → DB 加密凭据兜底 → 无凭据报 missing_api_key**）。
- **保存 Key 后下一次刷新即生效**（无需重启）：实测 zhipu snapshot
  用 DB Key 打真实智谱 API 得到 401（假 Key），而非 missing_api_key。
- prewarm 账号同样走 resolver。

### API（凭据路由，PLAN §19 认证覆盖）

`GET /api/v1/credentials`（无明文列表）/ `PUT /{id}`（label+Key，保存
后响应与日志均无 Key）/ `DELETE /{id}`（幂等）。master key 缺失时不挂
路由（fail-safe）。

### Web：订阅额度页启用

zhipu/kimi 账号卡：已配置状态 + 标签/更新时间、配置/更换/删除 Key
（password 输入不回显）、保存后提示"下一次刷新即生效"。

## Phase 5 补充（2026-11-14）：语音 LLM 接 Model Router

- `ai.Router.Complete(ctx, model, messages)`：非 HTTP 调用入口（内部域
  复用）。resolve 前缀路由 → mock upstream 走本地 `mockReplyText`（与
  HTTP 代理路径共用文案生成），真实 upstream 强制 stream=false POST +
  JSON 解析 choices[0].message.content。
- `voice.MockInteractor.Respond` 注入 Router（main 装配
  `SetRouter(chatRouter)`）：system prompt 约束口语化 1~3 句（TTS
  朗读友好）；**模型失败回退本地 echo**——语音交互永不因 LLM 挂而中断。
- e2e 实测：interact 回复从 voice 自拼文案变为 Router 生成
  （`[mock] 你说：现在几点了…`），WAV 时长随回复长度变化。
- 配真实 provider 后（`CHAT_UPSTREAM_*`）语音回复即为真实模型输出，
  voice 域零改动。

## Phase 5 补充（2026-11-14）：语音链路接 Bridge（ASR→LLM→TTS 闭环）

### Bridge（internal/voice）

- **一步式协议**：`POST /api/v1/voice/interact` body=原始 PCM
  （audio/l16;rate=16000），响应=WAV（audio/wav）+ 两个文本响应头
  （`X-Voice-Transcript`/`X-Voice-Reply`）——设备端一次往返拿齐
  播放音频与展示文本。
- `Interactor` 接口三方法（Transcribe/Respond/Synthesize）= 真实
  ASR/LLM/TTS 的唯一替换点；mock 实现：转写按请求轮换固定话术、
  回复 echo 风格、TTS 生成按字符时长的 440Hz 包络哔声 WAV（44 字节
  标准头手工拼装，16kHz/16bit/mono）。
- 单测 3 组：stub 流水线（含 WAV 头逐字段校验）/ interact e2e /
  空音频 400。curl 冒烟：0.5s PCM → 200 + RIFF 313KB + 文本头 ✅
- Respond 当前 mock echo；接真实 LLM 时换 internal/ai Router 的
  非流式调用（TODO 注释已留）。

### 模拟器

- `http_post_raw_stream`：任意 body 长度 + 自定义 Content-Type +
  **双响应头捕获**（首版只支持一个头，voice 需要 transcript+reply
  一次拿齐，扩展为 header1/header2 双参数）。
- `ds_voice_interact`：bridge 实现 POST PCM + 双头解析（WAV 体丢弃，
  字幕推进；真机 Phase 6 交 audio ops 播放）；mock 实现本地伪转写。
- `voice_session` 新增 `vs_notify_engine_done`（Thinking→Speaking 由
  引擎驱动；ops 存在时 Thinking 超时 15s 兜底防卡死，纯状态机模式
  保留固定时长）。play 启动信号随 done 通知走。
- `ui_voice` 真实链路：Listening 期间 on_pcm 帧累积录音缓存（8s 上限），
  PTT 松开进 Thinking 时 pthread 调 interact，完成经 poll timer 回 UI
  （显示"听到: …" + vs_set_reply + engine_done → Speaking 字幕）。

### 排查记录

- 数据源追加函数定义在 impl 结构体之后：前置声明先被脚本插进了
  函数体（block scope static 报错），再插到注释前仍在引用点之后，
  最终放文件头部静态变量区。教训：**追加式修改的函数要在插入点之前
  有声明**，声明位置= 引用点之前 + 文件作用域。
- 编辑事故两处（垃圾 token / switch 结构残留）均被编译器当场抓获；
  连续编辑后必须 vet/build 而不是攒到最后。

## Phase 5 补充（2026-11-14）：Audio Manager 骨架 + Voice Session

按 PLAN §11 新分层落地语音地基（`simulator/lvgl-pc/src/voice/`）：

### voice_session（引擎无关，LVGL 零依赖）

- 状态机 `Idle → Listening → Thinking → Speaking`（+Error 1.2s 自动回落）；
  推进唯一入口 `vs_tick()`（宿主 50ms 定时器），计时用 clock_gettime
  （组件不依赖 LVGL）。
- **独占租约** `vs_acquire/release`（ADR-003）：同刻只授予一个引擎；
  释放时自动回 Idle 并停采集/播放。
- 音频硬件经 `vs_audio_ops_t`（start/stop_capture、非阻塞 play +
  `vs_notify_play_done`、cancel）——固件 real adapter（I2S+ES8311/ES7210）
  同签名替换，业务/UI 代码零改动。
- listener 三回调：on_state / on_speak_progress（UTF-8 字符粒度字幕）/
  on_pcm（波形可视化 TODO）。
- PTT 只做 Push-to-Talk；VAD/连续对话/opus 为真机后续项（PLAN §11.2）。
- 设计坑：THINKING→SPEAKING 曾把回复文本强转 PCM 传 ops->play（语义
  错误）且 mock play 立即返回导致 SPEAKING 永久卡住——改为 play(NULL,0)
  仅作启动信号 + **字幕进度始终兜底推进**（真实 TTS 完成经
  play_done 提前打断回 Idle）。

### audio_mock

无真实音频的 ops 实现：采集置位标志、播放立即完成。装配点
`audio_mock_ops()`（固件侧换 `audio_real_ops()`，见 audio_mock.h 注释）。

### ui_voice（Muse App 位演示页）

光环 arc 随状态变色（绿=聆听/黄=思考/紫=回答/红=错误）+ 状态文字 +
"按住说话"PTT 大按钮（PRESSED/RELEASED 两事件）+ 朗读字幕。
页面上方显示当前租约引擎名。Xiaozhi 真机接入时复用本页模式。

### 构建

新增 src/voice/ 目录 include 路径；`audio_mock_ops` 需独立头文件声明
（隐式声明 -Werror）。**writing-failure 记录**：连续两次长 C 文件
Write 尾部截断成占位符 + 一次写到错误路径，全部靠编译错误发现；
长文件落盘后必须立即重读验证尾部完整性。

## Phase 5 补充（2026-11-14）：模拟器聊天流式

- `http_client` 新增 `http_post_json_stream`：POST 后逐字节读响应头
  （`read_line` 区分 EOF 与空行——两者都返回 0 是首版 bug），2xx 才
  回调；**chunked 帧解析**（hex 大小行 → 定长读 → CRLF；Bridge 的
  flush 响应都是 chunked 编码，不解析会把帧边界字符混进 SSE 数据）；
  非 chunked 直接整读。超时沿用 SO_RCVTIMEO。
- `data_source` 聊天接口升级 `ds_chat_send_stream`（delta 回调可为
  NULL）；mock 实现 UTF-8 字符级"伪流"；bridge 实现 `sse_parser_t`
  （跨 chunk 残帧缓冲、`data:` 行提取、`[DONE]` 跳过、full 拼接）。
- `ui_chat` 流式渲染：worker 的 delta 回调**只写互斥缓冲**（LVGL
  非线程安全），poll 定时器每帧把未刷新部分拼进占位气泡
  （LVGL 9.2 无 `LV_LABEL_LAST`，读全文拼接回写）；结束时用完整回复
  落历史，防 UI 拼接与 worker 拼接不一致。
- e2e：独立 C 测试程序直连 Bridge（不含 LVGL）实测 46 chunks、
  帧边界正确、`[DONE]` 终止。文本编辑环节曾把占位符写进源码+切错
  行号，靠编译错误与读文件复核修复——教训：大段改写用脚本替换后
  必须检查两处接缝。

## Phase 5 开工（2026-11-14）：Bigfish Chat 文字链路

三端打通：Bridge Model Router（OpenAI 兼容透传）→ Web 调试页（SSE 流式）
→ 模拟器聊天 App（非流式简化）。默认 Mock upstream，全链路无 Key 可演示。

### Bridge（internal/ai）

- `Router`：`model = "<provider>/<model>"` 前缀路由（大小写不敏感）；
  无前缀用默认 provider；未知前缀 400。**无状态透传**——Bridge 不解析
  messages，只改写 model + 注入 Authorization，upstream 响应（JSON/SSE）
  流式回传；客户端断开经 `request.Context().Done()` 取消 upstream。
- upstream 注册：环境变量 `CHAT_UPSTREAM_<ID>="URL,KEY"`（如
  `CHAT_UPSTREAM_ZHIPU="https://open.bigmodel.cn/api/paas/v4,KEY"`，
  DeepSeek/GLM 开放平台/豆包/OpenAI 均 OpenAI 兼容）；`CHAT_DEFAULT_PROVIDER`
  指定默认；**无配置自动 mock 兜底**。
- mock upstream（`mock://` 特判）：echo 风格回复；stream=true 按
  30ms/字符推 SSE（帧结构与 OpenAI chunk 一致），false 返回完整 JSON。
- 路由：`GET /api/v1/chat/providers`、`POST /api/v1/chat/completions`
  （body 上限 1MB）。
- 单测 4 组：路由解析/JSON 改写/SSE e2e/非流式 e2e。
- 测试坑：SSE 按字符分帧，`Contains("你好")` 断言必然失败——必须
  拼接各 delta 后断言。

### Web（/chat 调试页）

fetch ReadableStream 手动解析 SSE（`data: ` 帧按空行切分、残帧缓存），
AI 气泡逐字追加；provider 下拉来自 `/chat/providers`。

### 模拟器（大肥鱼聊天 App，替换占位）

- 气泡流（用户紫/AI 卡片色）+ textarea + 发送按钮；SDL 键盘输入
  （main.c 补 `lv_sdl_keyboard_create`）。
- **网络在 pthread 执行**，UI 100ms 定时器轮询结果——绝不阻塞渲染
  （首版曾在定时器回调里同步请求，LLM 30s 会冻死 UI，重写）。
  App 退出经 `on_exit` 停轮询；请求中退出安全（写静态缓冲）。
- 历史静态缓冲 16 条 FIFO，退出重进保留；Phase 5 简化为非流式。
- 构建坑两个：cJSON include 不能挂 `cjson` 目标（其 install/export
  规则触发 "prefixed in the build directory" 生成错误），改挂
  gadget_sim PRIVATE；且必须在 `add_executable` 之后。

## 电源与电池（2026-11-14 第五批）：全局状态栏 + 设备信息电源卡 + power_manager

**全局状态栏**（ui_statusbar.c，lv_layer_top overlay 点击穿透）：所有屏幕
顶部统一 Wi-Fi + 电池（五档图标 ≥80/60/40/20/<20；色档 20/10；充电绿
闪电；USB 供电无电池显示 USB 图标不伪造 100%）。规则与低电策略见
PLAN §13.3（<15% 提醒、<5% 节能，用户可调）。

**设备信息页**新增"电源与电池"卡（电压/电量/充电状态/USB，模拟器
mock 演示值）。**数据层**：gadget_state_t 补 battery_present/usb_connected/
charging/battery_voltage_mv；`GADGET_DEMO_BATTERY="pct,mv,idle|charging|usb|none"`
场景钩子；`GADGET_DEMO_SETTINGS_PAGE=device` 直达子页钩子。

**固件**：`firmware/components/power_manager/`——battery.h 统一接口
（battery_get_status）+ battery_axp2101.c（0x34 探测 + 寄存器 TODO 表，
bring-up 按数据手册核对）+ power_manager.c（esp_timer 轮询 30s/15s、
状态突变回调、低电策略钩子）。main.c 接入（PMU 探测失败仅告警不阻塞）。

踩坑：

- **lv_snapshot 不含图层**：快照只渲染传入对象子树，lv_layer_top 上的
  状态栏 overlay 拍不到——snap 改为屏幕+图层双快照 alpha 合成。
- **LVGL 内存池耗尽**：屏幕与图层两张 921KB 快照超出 1MB 池，第二张
  lv_snapshot_take 静默返回 NULL（状态栏"消失"根因）——改 lv_draw_buf_init
  包裹 static 系统缓冲（snap_take），绕开池分配。
- **idf v6 i2c 字段改名**：i2c_device_config_t 的 dev_addr → device_address。
- **设备信息硬件卡豆腐**：含中文的值（"双核 LX7 · 240 MHz"）误用
  montserrat 字体；cn 字体含 ASCII range（0x20-0x7F）可整行用 cn_14。
- Write 长文件时路径少一个连字符，头文件落到了隔壁目录（编译 fatal
  才暴露）；同名字幕 cache-busting 也会掩盖"没重编"——构建后用
  nm/像素数验证产物为准。

## UI 精修（2026-11-14 第四批）：四页按定稿图对齐 + 离屏截图钩子

**桌面**：彩色磁贴（三 AI 头像 + 六彩色符号，网格序按定稿 3×3）；
canvas 预渲染波浪渐变背景（RGB565 直写，4×4 Bayer 抖动消色带）；
状态栏时钟+日期。**监控页**：四列指标（CPU/内存/磁盘/网络，数据层
补 disk/net 字段）+ 在线胶囊 + 来源名截短。**订阅页**：5h 环改显
剩余 x.xh（window_seconds 换算）+ 本周额度卡 + 手动预热/刷新按钮列。
**设置页**：入口卡彩色圆角图标块 + 子页标题居中（原未 align 与
back 叠位）。

**GADGET_SNAP 离屏截图钩子**（main.c + LV_USE_SNAPSHOT）：
`GADGET_SNAP=xx.ppm[,ms]` 延时快照当前屏后自动退出，配
`SDL_VIDEODRIVER=dummy` headless 跑——UI 验收从"screencapture 抓
真窗口"（受真实鼠标/遮挡/分析器误读干扰）升级为确定性逐像素复现。

踩坑：

- **legacy 字段覆盖新解析**：subs[0].reset_week 被
  `= st->zhipu_reset_week`（从未更新的 legacy 字段）回写清 0，
  表现为"重置 01-01 08:00"（epoch）。定位：字节级同 hash 对比 +
  临时 fprintf——肉眼"代码看着对"时用二分打印最快。
- **screencapture 验收不可靠**：真窗口被用户交互/其他窗口遮挡，
  视觉模型分析缩放图误报（时钟"缺失"实为误读）；辅以像素计数
  （区域亮度统计）做客观仲裁，后整体换 GADGET_SNAP。
- **`A:` 盘符相对 CWD**：`A:simulator/lvgl-pc/assets/...` 路径要求
  从仓库根启动模拟器，从 simulator/lvgl-pc 目录启动则头像/GIF
  全部加载失败（表现为空磁贴）。
- **RGB565 渐变色带**：5-6-5 位深下渐变呈块状条带，Bayer 抖动 ±2 解决。
- **长 URL 钻到按钮下**：监控页"来源 bridge(http://...)"一行与右上
  刷新按钮同高，超出按钮左缘的部分被盖住（误读为截断）——显示层
  截短到 `(` 前。

## Phase 3 收尾（2026-11-14）：WebSocket events 实时通道

### Bridge

- `internal/events/hub.go`：进程内事件总线。订阅返回缓冲 16 的通道 +
  幂等退订；发布扇出，**慢消费者丢帧不阻塞发布者**（客户端可 HTTP 拉
  全量兜底，事件只作"该刷新了"信号）。单测：pub/sub、幂等退订、
  慢消费者不阻塞。
- `GET /api/v1/events`（gorilla/websocket v1.5.3）：升级后纯服务端推送；
  20s ping 保活；**断开感知靠读循环**——连接被 hijack 后
  `r.Context()` 在 handler 返回时即取消，不能用作断开信号（首版
  踩坑：测试报 close 1006 unexpected EOF）。
- 推送点：`fetchAndStore` 成功/失败都发 `quota.<id>`（载荷=完整
  StoredSnapshot）；`GET /komari/nodes` 发 `komari.updated`。
- 认证：Bearer header 为主；浏览器 WS 无法自定义 header，`/events`
  额外接受 `?token=`（查询串可能被代理记录，仅限受控局域网，已注释）。

### Web

- `api/ws.ts`：自动重连（1s→8s 指数退避）、token 注入、JSON 解析。
- Dashboard：`quota.<id>` 事件直接 `applySnapshot` 应用载荷（省一次
  HTTP）；保存 Token 后 reload 重连（连接 URL 携带旧 token）。
- Monitoring：`komari.updated` 应用视图 + "● 实时"连接状态徽标。

### e2e 实测

- 直连 `ws://bridge/api/v1/events`：POST refresh → 客户端收到
  `quota.mock` 完整载荷 ✅
- 经 vite 代理：需在 proxy 配置加 **`ws: true`**（缺省不透传升级请求，
  表现为 dial 超时）→ 修复后同等通过 ✅
- Go 临时客户端放 `/tmp/wsclient`（Node 18 无原生 WebSocket）。

## Phase 4 补充（2026-11-14）：智谱预热全链路

按 PLAN §5.6/§15.2 规范三端打通：

### Bridge（internal/quota/zhipu/prewarm.go）

- `Prewarmer`：幂等 Key（15min 记忆）→ 冷却（10min，跳过不占冷却）→
  周额度 ≥95% 保护 → 轻量请求 → 前后快照对比，全程记录。
- 轻量请求 = `POST /api/paas/v4/chat/completions`（max_tokens=1），
  模型默认 `glm-4-flash`（`ZHIPU_PREWARM_MODEL` 可覆盖；模型名需
  真实账号验证）。非订阅额度接口，但计费影响需实测确认。
- 结果状态机：`executed / failed / skipped_low_quota / skipped_cooldown /
  duplicate`；`failed` 也记录 before。
- `POST /api/v1/quota/zhipu/prewarm`：未配 Key → 503 provider_disabled
  （curl 实测）。单测 6 组全过。
- ⚠️ UI 与文档明确：预热 **不承诺重置 5 小时窗口**。

### Web（Dashboard 智谱卡）

预热按钮两段式：首次点击进入确认态（显示"模型 1 token 请求"），
再点才发起（`crypto.randomUUID()` 幂等 Key）；结果区展示 5 小时/周
额度 before → after + 耗时 + 免责说明。

### 模拟器（智谱额度页）

- `http_client` 新增 `http_post_json`（GET 顺带重构为
  `http_request` 公共路径）。
- data_source 新增 `ds_prewarm_zhipu`：bridge 实现 POST Bridge 并把
  status/message 透传给 UI；mock 实现本地模拟（无网络）。
- 额度页"预热"按钮 → 自建确认弹窗（**不用 lv_msgbox**：LVGL 9.2
  换了 API，`lv_msgbox_create` 只收 parent，旧签名编译失败）→
  确认后执行并把结果写到更新行。

## Phase 4 记录（2026-11-14）：Komari 监控链路

三件套全部打通（Bridge 聚合 → Web 卡片 → 模拟器设备页），全链路
e2e 实测（模拟器 bridge 模式拉 quota/kimi/komari 各恰 1 次）。

### Bridge（internal/komari）

- `Source` 接口（真实 Client / MockSource）；`Service` 30s TTL 缓存 +
  失败保留旧快照标 stale（与 quota 域同语义）。
- 真实 Client：`GET {KOMARI_URL}/api/nodes`（**C 级社区观察接口**，
  fixture 按社区记录整理，实际部署后必须对照版本验证）；
  字段名宽松解析（uuid/id、cpu/cpu_percent、mem/mem_percent…）、
  百分比 ≤1 视为比例归一；未知结构 → `unsupported_response`。
- Mock：6 台伪随机队列（30 秒粒度波动、偶尔掉线）。
- 路由：`GET /api/v1/komari/nodes`（`?refresh=1` 强制）；
  `KOMARI_URL`/`KOMARI_TOKEN` 启用真实源，默认 Mock。
- 单测 5 组：wrapped/顶层数组/别名/比例归一/未知结构/带鉴权 Fetch。

### Web（Monitoring 页）

服务器卡片：在线点 + CPU/内存/磁盘条（阈值配色与额度一致）+
上下行 + 运行时长 + 上报时间；stale 警示；Bridge 30s 节流说明。

### 模拟器（ui_monitor.c，替换占位页）

紧凑卡片列表（flex column 滚动）：名称/在线点/CPU+MEM 迷你条/
运行天数；刷新按钮 = ds_refresh + 整页重建（无状态残留）。
data_source 扩展 `gadget_server_t servers[6]`；bridge 实现新增拉取
`/api/v1/komari/nodes`。

### 排查记录

- e2e 首跑 0 请求：`go run` 冷编译 >2s，`sleep 2` 就启动模拟器导致
  连接被拒。联调用"轮询端口就绪"再启动客户端（已入脚本习惯）。

## Phase 2 记录（2026-11-14）

### Web 管理后台正式化 — ✅ 通过

- **Tailwind 3.4**（Node 18 兼容；Tailwind 4 需 Node 20+）+ 设计变量
  与设备 UI 对齐（`tailwind.config.js` 的 colors = ui_theme.h 同款色板）。
- **设备 API Mock 层** `web/src/api/device.mock.ts`：接口签名按
  PLAN §9.4 预定（status/config/wifiScan/wifiConnect/proxy/proxyTest/
  provisionInfo），带模拟延迟；固件侧设备 HTTP Server 就绪后只替换
  实现、页面不动。
- **Network 页**：Wi-Fi 扫描列表（信号条/加密类型）+ 连接表单；
  HTTP 代理卡（Direct/HTTP CONNECT、Basic 认证、密码不回显留空=保留、
  连通性测试）。
- **配网流程原型页** `/provisioning`：三步向导（连热点 → 选 Wi-Fi →
  完成）+ SoftAP 二维码（qrcode 库；**内容只有 URL**，热点密码只存在
  设备屏幕上，PLAN §9.3 安全规则）。
- **Bridge Token 认证**（PLAN §19 首项）：`BRIDGE_TOKEN` 环境变量启用，
  全部 `/api/v1/*` 要求 Bearer；实测无 Token 401 / 带 Token 200。
  Web 端 Dashboard 提供 Token 输入（仅存 localStorage，不进 URL/日志）。
- 响应式：<640px 顶部横向导航（PLAN §9.1 断点）。
- 构建验证：`vue-tsc --noEmit && vite build` 通过（CSS 13.9KB gzip 3.3KB）。

### Phase 2 遗留

- 手机端真机浏览器（iOS Safari/Android Chrome）实测。
- 设备设置页（显示/音频/电源）未做（设备 API 已有 config 接口预留）。
- Provider 账号配置 UI（等 Bridge 加密凭据存储）。
- 浅色主题（PLAN §8.1 说可选，暂只有深色）。

## Phase 1/3 补充记录（2026-11-14 第二批）

### CJK 字体 + 设备 UI 中文化 — ✅ 通过

- UI 文案全部中文化（桌面 9 个 App 名、额度页标签、占位页、日期"周X"）；
  数字/时钟/图标仍用 Montserrat（混排，两套字体并存）。
- 字体：Noto Sans SC（google/fonts variable TTF，**OFL 协议**，可嵌入
  固件分发）→ `scripts/gen_cn_font.sh` 扫描 UI 源码提取字符集
  （273 个非 ASCII 字符）+ lv_font_conv 生成 12/14/16/20 四字号
  （4bpp、不压缩、无 kerning），产物提交进
  `simulator/lvgl-pc/src/ui/fonts/`。
- 踩坑：lv_font_conv（当前 npx 版）不支持 `--lv-version`；符号名要用
  `--lv-font-name lv_font_cn_N` 显式指定（默认取文件名，会与
  `LV_FONT_DECLARE` 声明对不上）。
- 固件移植注意：四个字号共 ~960KB 源码（位图数据），进固件时按需裁
  字号或开 `LV_USE_FONT_COMPRESSED` 重生成。

### Bridge 快照 SQLite 持久化 — ✅ 通过（Phase 3 项）

- `internal/storage/`：modernc.org/sqlite（**纯 Go 无 cgo**，钉 v1.38.0
  以兼容 go 1.25——最新 v1.62 会把 go.mod 顶到 1.26）。WAL 模式、
  MaxOpenConns=1 防 lock。
- `quota.Store` 注入 `Persister` 接口：SaveSuccess/SaveError 落库
  （失败不影响内存态），启动 `AttachPersister` 恢复历史快照
  （损坏行跳过、不整体失败）。
- `-db` 参数 / `BRIDGE_DB` 环境变量，默认 `data/gadget.db`；传空串
  纯内存。
- 单测：save/reload roundtrip（stale 语义、窗口数据）、损坏行容错。
- 冒烟实测：启动→抓取→关进程→重启→GET 直接返回缓存快照
  （日志 `snapshot cache restored`）。

## Phase 1 记录（2026-11-14）

### 模拟器 app_manager + data_source — ✅ 通过

新增：

- `src/ui/app_manager.[ch]`：App 注册表（id/名称/图标/enabled/build/on_exit），
  打开→构建→转场，返回→转场回 Home→**动画结束自动销毁屏幕**
  （`lv_screen_load_anim` 的 `auto_del`，对齐 PLAN §4.3 单前台资源模型）。
- `src/data/`：`data_source.h` 为 UI 唯一数据入口；mock 实现复用伪随机数据，
  bridge 实现经零依赖阻塞 HTTP GET（`http_client.c`）+ cJSON 解析
  `/api/v1/quota/{provider}/snapshot`。
- 桌面网格与额度页全部改走注册表/数据源；环境变量
  `GADGET_DATA_SOURCE=bridge|mock`、`GADGET_BRIDGE_URL`、
  `GADGET_BRIDGE_PROVIDER` 切换。

验证：

```bash
cmake --build simulator/lvgl-pc/build -j        # ✅
./simulator/lvgl-pc/build/gadget_sim            # mock 模式 3s 存活 ✅
GADGET_DATA_SOURCE=bridge GADGET_BRIDGE_URL=http://127.0.0.1:8081 \
GADGET_BRIDGE_PROVIDER=mock ./simulator/lvgl-pc/build/gadget_sim
# bridge 模式 4s 存活 ✅；Bridge 日志命中 /api/v1/quota/mock/snapshot 恰 1 次
# （首刷成功后走缓存，不再重复请求）
```

踩坑记录：

1. **`ui_init` 漏 `lv_screen_load(launcher)`** → 时钟回调因
   "非活动屏幕"守卫全部早退，模拟器白屏且从未请求数据源。
   教训：白屏 + 0 请求一起出现时，先查屏幕有没有 load。
2. `go run` 起的 Bridge 用 `kill $!` 杀不干净（子进程残留占端口），
   联调后用 `lsof -ti :PORT | xargs kill` 兜底。
3. macOS 系统 python3.10（python.org 版）证书损坏 → IDF install.sh 失败；
   `PATH=/opt/homebrew/bin:$PATH ./install.sh esp32s3`（brew python3.13）绕过。

### 数据流缓存语义

ds_refresh 成功后 `updated_unix` 置位，ds_state 不再自动重拉；
当前唯一的周期刷新入口是额度页手动 Refresh 按钮（阻塞 ≤1.5s，
固件侧 TODO 换 esp_http_client 异步 + 事件回调）。

### Bridge：Kimi Code Adapter（P1）— ✅ 通过

- `internal/quota/kimi/`：`GET {base}/coding/v1/usages`，Bearer 专用 Key；
  `usage` → 月度/总量池，`limits[].window duration=300 & TIME_UNIT_MINUTE`
  → 5h 窗口；字符串数值转 float，used/limit 推导百分比（已用与剩余分开）；
  未知结构 → `unsupported_response`。Trust B。
- fixture 单测 3 组（窗口映射/未知结构/缺 Key）全过；`KIMI_API_KEY`
  / `KIMI_BASE_URL` 存在时才注册（默认关闭）。
- 模拟器 bridge 数据源同步改为双 provider 拉取：主 provider（默认 mock）
  → Zhipu 页；`/api/v1/quota/kimi/snapshot` → Kimi 页，未配 Key 时 404
  静默忽略（实测请求计数各 1，模拟器存活）。

## 遗留 TODO（按优先级）

1. **模拟器人工目视验收**：跑 `./simulator/lvgl-pc/build/gadget_sim`
   检查 Launcher 布局（3×3 网格/状态栏/额度环）。
2. 模拟器 UI 暂用英文标签 — 需要 CJK 点阵字体子集
   （lv_font_conv + 思源黑体，模拟器与固件共用）。
3. Web 握手 Bridge 的浏览器端真实验证（本次只用 curl 验证代理链路）。
4. Bridge 快照持久化（现为内存，SQLite 见 PLAN §15.3）。
5. Bridge Web API 认证（Phase 2，PLAN §19）。
6. 智谱/Kimi Adapter 用真实账号验证（现仅 fixture；响应结构可能随
   套餐/区域不同，需按 PLAN §6.1 规则更新 fixture）。
7. 固件烧录与板级验证（设备未到手，Phase 6）。
8. 真机验证清单见 [HARDWARE.md §4](HARDWARE.md#4-待真机验证清单phase-6)。

## 快速命令

```bash
scripts/check.sh                    # 全仓库检查（固件无 IDF 自动跳过）
bridge/run.sh                       # Bridge 开发服务器 :8081
cd web && npm run dev               # Web 开发服务器（/api 代理到 :8081）
./simulator/lvgl-pc/build/gadget_sim  # 480×480 模拟器
```
