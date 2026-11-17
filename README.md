# Waveshare ESP32-S3 33969 AI Gadget

基于 [Waveshare ESP32-S3-Touch-AMOLED-2.16（SKU 33969）](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm)
的社区定制多应用 AI Gadget。非 Waveshare 官方固件。

**当前版本 v1.0.0（2026-11-17）**——固件 / Bridge / Web 后台统一版本号，
配网 / 语音 / 字库 / 省电全链路真机验收通过。

![UI 定稿概念图](assets/ui/concepts/assistant-ui-final.png)

> **UI 风格基准**（2026-11-14 定稿，不可变更）：三 AI 对话页
> （顶栏头像+在线点 / 中央角色形象 / 中央麦克风大按钮+左右波形）+
> 桌面 9 宫格（三 AI 形象头像 + 彩色符号磁贴）。设备端与 Web 端
> 所有界面均以本图为准；实现对齐项见 [docs/UI_DESIGN.md](docs/UI_DESIGN.md)。

> 完整规划见 [PLAN.md](PLAN.md)。硬件调研见 [docs/HARDWARE.md](docs/HARDWARE.md)，
> 系统架构见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，
> 关键决策记录见 [docs/DECISIONS.md](docs/DECISIONS.md)，
> 电源管理方案见 [docs/POWER_MANAGEMENT.md](docs/POWER_MANAGEMENT.md)，
> 开发环境搭建见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。
> 真机刷写与驱动验证手册见 [docs/BRINGUP.md](docs/BRINGUP.md)。

## 仓库结构

```text
waveshare-esp32-s3-33969-ai-gadget/
├── firmware/          # ESP32-S3 固件（ESP-IDF v6.0.x + LVGL，真机 bring-up 已完成）
│   └── components/power_manager/   # AXP2101 电池/充电 + 轮询策略 + 充电屏保钩子
├── bridge/            # Go Bridge：额度/凭据/AI 路由/语音（ASR/LLM/TTS、音色试听）
├── web/               # Vue 3 管理后台（总览 + 12 个配置页，手机/PC 响应式）
├── simulator/lvgl-pc/ # LVGL PC 模拟器（480×480，主 UI 开发环境，含中文字体子集）
│   └── assets/icons/  # 桌面应用图标（自备素材 + 缺位回退）
├── assets/            # 设计素材（概念图、吉祥物，非官方资产）
├── docs/              # 硬件、架构、决策、开发、bring-up、小智协议调研
├── scripts/           # check.sh 全仓检查 / gen_cn_font.sh 字体子集 等
└── PLAN.md            # 项目总规划
```

## Bridge 是什么（为什么要自部署）

设备本身只负责界面、录音与播放——**所有 AI 能力与密钥都在 Bridge**：

| 能力 | 设备端 | Bridge（自部署在你的电脑或服务器） |
|------|--------|-----------------------------------|
| 聊天 | 文本上屏 | 转发 AstrBot（人设/模型路由、思考链过滤） |
| 语音对话 | 录音上传、播放回传音频 | ASR 识别 → LLM 回复 → TTS 合成；小智 App 代连 xiaozhi.me（OTA/WebSocket/Opus）；Muse 加密会话 |
| TTS 朗读 | 只播放 | 全部合成（Edge-TTS/火山/官方小智直通） |
| 天气/订阅额度 | 只显示 | 抓 Open-Meteo（缓存保温）+ 拿各家 Key 抓供应商接口 |
| 配置下发 | 开机拉取应用 | 加密落库并即时生效（机器人/音色/订阅/排布/按键） |

**为什么必须自部署**：
1. **安全**——API Key / Token 只允许存 Bridge（AES-256-GCM 加密落库），不进固件；
2. **资源**——ESP32 跑不了 Go，也装不下 TTS/Opus/各家协议（app 分区仅剩 5%）；
3. **演进**——加 AI 能力只改 Bridge，设备零改动（订阅/小智/会话隔离这些功能上线时设备一次都没重刷）。

**部署在哪**：任意常驻主机——开发调试用本机电脑（Mac 直接 `./run.sh`），
正式使用部署到服务器/NAS/树莓派（一个进程一个端口，Web 后台也由它托管）。
部署细节见 [docs/DEPLOY.md](docs/DEPLOY.md)。

**多地址切换（开发/正式共用一台设备）**：设备「设置 → 网络与连接 →
Bridge 地址」是一个**地址簿**——可保存多个地址、点一下即切换
（开发时连本机 `http://192.168.x.x:8081`，日常切服务器 `http://<域名>:8081`），
切换后对话/订阅立即走新地址，无需重刷固件。

## 新用户上手（从零开始）

按顺序走一遍（每步都有对应的详细文档链接）：

0. **前置**：给原厂固件做全片备份（不可找回，见
   [BRINGUP §1.0](docs/BRINGUP.md)）；装好 ESP-IDF v6.0.x、Go、Node（开发机）。

1. **（可选）Muse 授权**：只有想用 Muse App 才需要。Muse 授权必须先用
   **官方 Muse SDK 固件**配对一次——刷官方 SDK → 手机 Muse App 配对 →
   `esptool read-flash 0x11000 0xC000 nvs.bin` 导出并记下
   `access_token`/`refresh_token`/`node_id`/`noise_host`（后面 Bridge 要用）。
   详见下文《Muse 配对与恢复》。

2. **部署 Bridge**（二选一）：
   - **正式**：一台常驻服务器/NAS —— 目标机构建 `go build -tags nolibopusfile`，
     拷 `bridge.env`（`BRIDGE_MASTER_KEY`）与 `data/gadget.db`，`-webdir` 托管前端
     （自动探测），systemd 常驻。完整步骤见 [docs/DEPLOY.md](docs/DEPLOY.md)。
   - **开发**：本机电脑（Mac/Linux 均可）：
     ```bash
     cd bridge && go build -tags nolibopusfile -o gadget-bridge ./cmd/gadget-bridge
     ./run.sh                      # :8081（读 bridge.env）
     cd ../web && npm install && npm run dev   # 开发热更新用 :5173；:8081 也托管构建产物
     ```
   - **AstrBot 也要自部署**：聊天/语音回复的后端就是 AstrBot（任意能跑
     Docker 的机器，也可与 Bridge 同机）。部署后在 Web「AstrBot」页填
     服务地址与 Token，设备端即可选机器人实例。

3. **刷固件**：
   ```bash
   pkill -9 -f esp-idf-monitor      # 否则串口被占用，刷写必失败
   cd firmware && idf.py build
   idf.py -p /dev/cu.usbmodem1101 flash
   ```
   `firmware/main/wifi_secret.h` 只是出厂兜底，**自己开发推荐不填**（用第 4 步
   运行期配网）；**给别人打包分发时推荐预填**（对方刷完即用，跳过配网，
   见下文《给他人打包固件》）。

4. **设备配网**：设备「设置 → 网络与连接」——Wi-Fi 设置（扫描→选网→软键盘输密码，
   存 NVS 重启记住）+ Bridge 地址（填第 2 步部署的地址；多地址可保存切换）。

5. **打开后台完成配置**：手机扫设备「设置 → 网络与连接 → 管理后台」二维码，
   或直接开 `http://<Bridge 主机>:8081` —— 按页面依次配置：
   AstrBot 实例（/bots）、语音 Key 与音色（/voice，可试听）、订阅实例与自动刷新（/subscriptions）、
   小智官方服务（/xiaozhi）、Muse 凭据（/muse，粘贴第 1 步导出的四项）、外观排布（/appearance）。

6. **多环境**：在设备「Bridge 地址」地址簿里同时保存本机与服务器地址，
   开发调试切本机、正式使用切服务器，一键生效。

### 给他人打包固件（免配网开箱即用）

把 Wi-Fi 和 Bridge 地址在**编译期**写进固件，对方拿到 `ai_gadget.bin`
刷入即连网即连 Bridge，零设备端操作：

```bash
cd firmware/main
cp wifi_secret.h.example wifi_secret.h   # 该文件已 gitignore，不会入库
# 编辑 wifi_secret.h：
#   WIFI_SSID / WIFI_PASS     对方现场的 2.4G Wi-Fi
#   BRIDGE_HOST               你的 Bridge 地址（默认激活的那个）
#   BRIDGE_SEED_HOSTS         地址簿预置（逗号分隔；公网域名写明文 80，
#                             不要带 :8081——见下「地址端口注意」）
cd .. && idf.py build
# 交付 build/ai_gadget.bin，对方刷写：
#   esptool.py --chip esp32s3 write_flash 0x0 build/bootloader.bin \
#     0x8000 build/partition_table/partition-table.bin 0x20000 build/ai_gadget.bin
```

注意事项：
- **凭据安全**：`wifi_secret.h` 不进 Git；交付的 bin 内含 Wi-Fi 明文密码，
  只发给信任的人（NVS 运行期配网的凭据优先级高于编译兜底，对方之后
  可在设备上改成自己的网络）。
- **地址端口注意**：设备 HTTP 只支持明文 `http://`（无 TLS、不带
  Authorization）——公网 Bridge 必须开 80 明文端口且不能设
  `BRIDGE_TOKEN`；`BRIDGE_SEED_HOSTS` 里公网地址写域名不带端口。
- **后续可换**：所有预填项都能在设备「设置 → 网络与连接」里改
  （Wi-Fi 扫描重配 / Bridge 地址切换），编译兜底只在 NVS 从未配置过时生效。

### 日常使用

- 首页左右滑翻页，底部小圆点跟随；一屏 4 个 App，排布可在 Web「外观」里改。
- **AstrBot 对话**：点页面标题弹「选择机器人」，选中的实例决定文字与语音回复；
  Web 的 AstrBot 页也能「设为当前」。
- **语音**：按住底部麦克风说话，松开后依次显示 听 → 想 → 说，回复会朗读。
  转写 Key 与每个 App 的音色在 Web「语音设置」，每个音色旁可「▶ 试听」。
- **订阅中心**：设备订阅页显示 Web「订阅中心」配置的订阅实例
  （类型 + 名称 + API Key，选类型自动填官方端点；↑↓ 调顺序；页面底部可设
  设备订阅自动刷新间隔 1~60 分钟）。coding plan 卡显示剩余百分比环+重置
  倒计时+周额度（智谱含套餐到期，可预热）；按量计费卡（DeepSeek）显示余额。
- **文件**：管理 TF 卡（插卡即用）——浏览目录/进入/返回上级、删除文件
  （两击确认）、把文件上传到 Bridge 备份；「Bridge 文件」里可把 Web
  「设备文件」页上传的文件下载到 TF `downloads/`（≤2MB）。
- **番茄钟**：25 分钟专注 + 5 分钟短休息，每 4 轮进入 15 分钟长休息，
  自动循环；大圆环进度 + 大号播放/暂停按钮，阶段/统计放在环外（奶油色），
  开始/暂停、重置/跳过（104×44 药丸按钮）。允许后台计时（切到其他 App
  继续走，状态栏顶部显示剩余时间），今日/累计完成数存
  TF `data/pomodoro.json`。
- **TF 备份与缓存**（自动）：配网信息每次变更后写 `backup/provision.json`
  ——刷机/擦 NVS 后插同一张卡开机自动恢复；订阅/天气数据落 `cache/`，
  断网时显示最后一次数据；番茄钟统计落 `data/pomodoro.json`。
  **备份含 Wi-Fi 明文密码，TF 卡勿外借。**
- **小智**：在 Web「小智」页点「连接官方服务」，把 6 位激活码输入
  xiaozhi.me 控制台绑定后，设备小智 App 的转写、人设与音色全走官方。
  未绑定或官方服务失败时自动回落本机语音链路。

### 能拔 USB 线吗？

- **设备**：USB 只负责供电/串口日志，联网走 Wi-Fi。板子接了电池（或换充电器
  供电）就能拔线独立运行；**没接电池拔线即断电**。
- **AI 功能**：回复要经过 Bridge。Bridge 部署在常驻服务器时设备完全独立；
  若还在本机 Mac 上跑，Mac 关机/睡眠、Bridge 没起或不在同一 Wi-Fi 则对话失败
  （设备配网页可切到别的 Bridge 地址）。
- 刷机、看串口日志时必须接 USB。

### 打开后台

- **部署形态（推荐）**：Bridge 直接托管 Web 后台（一个进程一个端口），
  手机访问 `http://<Bridge 主机>:8081`——设备管理后台二维码就是这个地址。
  产物目录自动探测（`webdist/` 或 `../web/dist`，也可 `-webdir` 指定）。
- **开发形态**：Vite 开发服务器 `http://<Mac 局域网 IP>:5173`（热更新）。
- **公网部署注意**：设备端 HTTP 客户端**只支持明文 `http://`（无 TLS）**，
  也不发 `Authorization`——公网 Bridge 必须开 **80 或 8081 明文端口**
  （只开 HTTPS，或反代默认的 http→https 强跳，设备都连不上），且
  **不要设 `BRIDGE_TOKEN`**（设备会 401）。接口无鉴权时请用非默认端口 +
  防火墙/IP 白名单收窄暴露面。详见 [docs/DEPLOY.md §5](docs/DEPLOY.md)。

### 常见问题

| 现象 | 处理 |
|------|------|
| 设备订阅页是空的 | Web「订阅中心」添加订阅实例（类型+名称+API Key）——实例才是设备显示的卡片 |
| 设备连不上 Bridge | 设置→网络与连接：确认 Wi-Fi 已连接、Bridge 地址正确（改完立即生效）；串口看 `net probe: ... stage=3` = TCP 被网络挡住（访客网/企业网隔离，换能出网的 Wi-Fi/热点）；`curl http://<BridgeIP>:8081/api/v1/device/status` 验证服务在跑 |
| 设备对话一直失败 | Bridge 没起或不可达；换过网络就在设备地址簿里切换 Bridge 地址 |
| AstrBot 提示「模型暂不可用」 | Web「AstrBot」页点「测试」看真实错误；检查 AstrBot 服务与实例配置 |
| 有文字没声音 | 设备音量/静音；或 TTS Key 没配（Web 语音设置看「当前生效」）|
| 设备偶尔自动重启一次 | 启动期显示管线楔死，任务看门狗约 20s 自动恢复；连续发生再看串口日志 |
| 文件页提示「未插卡」 | TF 卡未插/格式不支持（需 FAT32）；插卡重启后启动日志应有 `sdcard: mounted` |
| 断网时订阅页有旧数据 | 正常：TF 离线缓存生效，更新时间显示为缓存写入时刻 |
| 长按电源键关机 | 长按约 2s 优雅关机；**插着 USB 时关机后可能立即重新开机**（VBUS 触发，硬件行为）——拔掉 USB 再关。固件卡死时长按 10s 硬关兜底 |
| 中文显示方块 | 字体过期：`scripts/gen_cn_font.sh` 后重刷 |
| 刷写失败 | 先 `pkill -9 -f esp-idf-monitor`，再确认 `/dev/cu.usbmodem*` |
| flash 余量告警 | app 分区只剩 5%；加功能前先裁剪字体/素材 |

### Muse 配对与恢复

Muse 云端授权**不能在本固件里完成**，必须先走一次官方流程：

1. 确认原厂全片备份存在，然后刷**官方 Muse Gadget SDK 固件**
   （`sdkconfig.muse-waveshare-s3-216`，ESP-IDF v6.0.3）。
2. 手机 Muse App 配对设备并完成授权（设备屏出现 `setup=done ws=up` 即成功）。
3. 导出 NVS：`esptool read-flash 0x11000 0xC000 nvs.bin`，取出
   `access_token` / `refresh_token` / `node_id` / `noise_host`。
4. **刷回本项目固件**，并重烧伴宠资源分区（`scripts/flash_companion.sh`）。
5. Web「Muse」页填四项凭据 → 保存 →「连通性测试」。

注意：本项目固件的 app 从 0x10000 刷入会覆盖官方 SDK 的 NVS 区——**配对信息
只在第 3 步的导出文件和 Bridge 里**，请妥善保存（建议存到私密目录，勿入 Git）。
凭据被 revoke/过期需重做第 1–3 步。实测记录见
[docs/HARDWARE.md §2.9](docs/HARDWARE.md) 与 [ADR-004](docs/DECISIONS.md)。

## 开发与检查命令

```bash
scripts/check.sh    # bridge go vet/build/test + web typecheck + simulator cmake + firmware idf.py build

# LVGL PC 模拟器（UI 主开发环境）
brew install sdl2
cmake -S simulator/lvgl-pc -B simulator/lvgl-pc/build
cmake --build simulator/lvgl-pc/build -j
./simulator/lvgl-pc/build/gadget_sim            # mock 数据源
GADGET_DS=bridge GADGET_BRIDGE=http://127.0.0.1:8081 \
    ./simulator/lvgl-pc/build/gadget_sim        # 连真 Bridge
# headless 回归钩子：GADGET_SNAP / DEMO_APP / DEMO_PAGE / DEMO_KEYS

# 中文字体（改字典/加字后必须重新生成）
scripts/gen_cn_font.sh
```

## 状态（v1.0.0 · 2026-11-17）

| 模块 | 状态 |
|------|------|
| 真机 bring-up（Phase 6 前半） | ✅ 全外设验证通过：I2C×6 / 显示 / 触摸 / 音频双向 / TF / RTC / AXP2101 真读数 / Wi-Fi→Bridge / 充电屏保 |
| 模拟器 UI | ✅ Launcher（多页/页点/图标）+ AstrBot 对话 / 小智 / 天气 / 额度 / 订阅 / 文件 / 番茄钟 / 外观 / 按键映射 |
| Bridge | ✅ 额度 + 凭据 + 订阅 + AstrBot 多实例（当前实例切换）+ 天气 + 设备文件 + 按键/排布 + 外观 + events(WS) + 语音链路（ASR/LLM/TTS、音色试听） |
| Web 后台 | ✅ 总览 + 12 个配置页：外观/网络与配网/代理/按键/订阅中心/AstrBot/语音设置/Muse/小智/天气/设备文件 |
| 固件 UI 移植（Phase 6 后半） | ✅ 主体已在真机运行（launcher/对话/设置/订阅/天气/文件/番茄钟；共享模拟器源码） |
| 稳定性（2026-11-17 收敛） | ✅ 绘制缓冲内部 RAM + 语音静态栈任务 + LVGL 心跳看门狗；内存策略对齐官方（ALWAYSINTERNAL=2048，LVGL 控件走 PSRAM）；配网链路四层加固（静态任务/PS 时序/SNTP 幂等/DMA 水位）；小智语音回合（公司 Wi-Fi + NAS Bridge）实测流畅 |
| 字库（2026-11-17） | ✅ GB2312 全集 6763 字全字号（12/14/16/20/24px，bpp4）并入主库——生僻字（魅/薰等）任意字号正常渲染；24px 大字库就绪待用；flash 5.79MB/11.94MB（54%） |
| 网络诊断与省电（2026-11-17） | ✅ 设备端一键诊断（门户劫持检测+认证页二维码 / 阻断型网络提示）；Wi-Fi 省电自动档（通电全速 / 电池按场景分档，息屏省电，1s 粒度热切） |
| 小智官方服务（xiaozhi.me） | ✅ 已接入并真网验证：Bridge 模拟设备（OTA 激活 + WebSocket + Opus，官方 TTS 直通），未绑定自动回落本机链路，见 [ADR-005](docs/DECISIONS.md) |
| TF 文件管理（ADR-010） | ✅ 文件 App（浏览/删除/上传/下载）+ 配网自动备份/恢复 + 订阅/天气离线缓存；真机 TF 挂载与备份写入实测，下载上限 2MB（1-bit SDMMC 吞吐待测） |
| 设备运行期配网 | ✅ 设置→网络与连接：Wi-Fi 扫描/软键盘输密码存 NVS + Bridge 地址热改（`ds_bridge_rebind` 即时生效）；`wifi_secret.h` 退为出厂兜底 |
| Bridge 服务化部署 | ✅ `-webdir` 托管 Web 后台（API+管理页一个端口）；`scripts/build-release.sh` + [docs/DEPLOY.md](docs/DEPLOY.md)（systemd/数据迁移/设备指向） |
| 设备端音乐 | ⏳ 规划中 |

里程碑与详细进度见 [docs/PROGRESS.md](docs/PROGRESS.md)。
所有未在真机验证的内容都在 docs 中以 TODO 标注，不声称已测试。

## 踩坑速查（改固件前先看）

| 坑 | 结论 |
|----|------|
| 绘制缓冲放 PSRAM | SPI 每块要在内部 RAM 分配同尺寸 bounce buffer，碎片化后必然失败 → 丢帧花屏 + flush 卡死。**现在固定内部 RAM 10 行双缓冲，别改回 PSRAM** |
| 语音线程动态栈 | 内部 RAM 碎片化时 12–16KB 栈 ENOMEM；栈放 PSRAM 会 `InstrFetchProhibited`。**用静态栈任务（.bss 14KB）** |
| 看门狗按「锁 BUSY」判 | 聊天页重绘慢时会误杀（心跳仍在推进）。**判据是 LVGL 心跳 15s 不推进**；TWDT 10s 自愈 |
| 字体压缩/回退 | `gen_cn_font.sh` 默认 RLE 压缩，必须开 `LV_USE_FONT_COMPRESSED`，否则空心方块；生成器自带 `.fallback = NULL` 会覆盖挂接。**当前不压缩、无 L2 回退，flash 余 5%** |
| 桌面重建 | `ui_launcher_rebuild` 前必须清页点数组（悬垂指针会 LoadProhibited）；波浪背景 buffer 要复用 |
| 开机拉远端配置 | Wi-Fi 未起时拉不到；`remote_cfg_task` 两段式：prefetch（不持锁）→ refresh（持锁应用+重建） |
| ES7210 按轮启停采集 | 播放期间无人读 mic → RX DMA 溢出后读数恒为零（转写永远同一句、rc=0 无报错）。**采集任务常驻，别按轮启停** |
| 主任务直发 QSPI 命令 | 背光 `tx_param` 与 LVGL 首帧 DMA 并发会撑死总线断言（实测 125s 后崩溃）。**发 polling 事务必须持 display_lock** |
| Bridge 重启 | 必须带同一个 `BRIDGE_MASTER_KEY`，否则凭据无法解密；开发机日志 `/tmp/bridge.log` |
| 局域网后台 | Bridge 监听 `:8081` 全部网卡；Vite 已 `host: true`；设备只连 Bridge，不连 Web |

完整记录见 [AGENTS.md「稳定性踩坑」](AGENTS.md) 与 [docs/HARDWARE.md §2.10](docs/HARDWARE.md)。

## 安全原则

- API Key / Cookie / OAuth Token 永不进入固件、日志、Git 和二维码。
- 供应商凭据与 AstrBot token 只保存在 Go Bridge（AES-256-GCM 落库、不回显）。
- 供应商订阅额度接口按可信度分级（A/B/C/D），未公开接口默认关闭。
- 原厂固件闭源且不可找回——**刷写前必须全片备份**（见 BRINGUP.md §1.0）。
