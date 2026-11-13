# 系统架构

> 本文定义三个工程（Firmware / Web / Go Bridge）的模块边界和接口约定。
> 总体规划见 [PLAN.md](../PLAN.md)；决策记录见 [DECISIONS.md](DECISIONS.md)；
> 硬件细节见 [HARDWARE.md](HARDWARE.md)。

## 1. 总览

```text
                          ┌────────────────────────────┐
                          │   Web 管理后台（手机/PC）    │
                          │   Vue3 + TS + Vite         │
                          └─────────────┬──────────────┘
                                        │ HTTP (LAN)
              ┌─────────────────────────┼─────────────────────────┐
              │                         │                         │
   ┌──────────▼──────────┐   ┌──────────▼──────────┐   ┌──────────▼──────────┐
   │ ESP32 本地 Web Server │   │   Go Bridge          │   │   外部服务            │
   │ (配网/设备设置)        │   │ (凭据/额度/监控/路由)  │   │ 智谱/Kimi/Komari/   │
   └──────────┬──────────┘   └──────────┬──────────┘     │ Muse/小智服务端      │
              │                         │                └──────────▲──────────┘
   ┌──────────▼─────────────────────────┴──────────────────────┐   │
   │                    ESP32-S3 Firmware                        ├───┘
   │   Launcher + Apps + 系统服务（常驻） + 硬件 HAL               │
   └─────────────────────────────────────────────────────────────┘
```

关键边界：

1. **凭据边界**：所有第三方 API Key/OAuth/Cookie 只存在于 Go Bridge。
   ESP32 与 Web 前端永不接触供应商凭据。
2. **数据边界**：订阅额度/服务器监控由 Bridge 聚合，设备只收摘要；
   设备本地配置（Wi-Fi/显示/音频）只存在于设备 NVS。
3. **配网边界**：首次配网 SoftAP + 设备本地 Web（`192.168.4.1`），
   只配置 Wi-Fi，不涉及任何云端凭据。

## 2. Firmware 分层

```text
┌─────────────────────────────────────────────────────────────┐
│ apps/（业务 App，同一时刻一个前台）                            │
│  launcher │ settings │ bigfish_chat │ xiaozhi │ muse │ ...  │
├─────────────────────────────────────────────────────────────┤
│ 系统服务（常驻）                                               │
│  app_manager      App 注册/切换/生命周期/前后台资源回收          │
│  notification     通知中心与角标                                │
│  device_config    NVS 配置读写                                  │
│  web_server       设备本地 HTTP API + 配网页                    │
│  wifi_provisioning SoftAP 配网流程                              │
├─────────────────────────────────────────────────────────────┤
│ 共享能力服务                                                    │
│  display_manager  显示会话/亮度/旋转（封装 BSP/自研 CO5300）      │
│  touch_manager    触摸输入                                      │
│  audio_manager    I2S/ES8311/ES7210、opus 编解码、缓冲、音量      │
│  voice_session    语音会话状态机/Push-to-Talk/连续对话/独占租约   │
│  network_manager  Wi-Fi/DNS/TLS/WS/HTTP/代理传输                │
│  storage_manager  TF/文件系统                                   │
│  power_manager    AXP2101/息屏/睡眠/电源锁                      │
├─────────────────────────────────────────────────────────────┤
│ hardware/（板级 HAL：33969 real / PC mock 双实现）              │
└─────────────────────────────────────────────────────────────┘
```

规则：

- App 之间不互相调用；跨 App 数据经系统服务或 Bridge。
- App 不直接触碰 I2S/显示总线；一律经共享能力服务。
- 语音引擎（xiaozhi/muse/bigfish 语音）通过 `voice_session` 独占租约
  获取音频流；App 失败退出时租约自动释放。
- `hardware/` 提供 `real`（BSP + 自驱 PMU/RTC/IMU）与 `mock`（PC 模拟）
  两套实现，业务层代码不感知运行环境（ADR-001）。

## 3. UI 代码共享（模拟器 ↔ 固件）

```text
simulator/lvgl-pc/src/
├── ui/                ← 纯 LVGL 9 代码，不含 IDF 专有 API
│   ├── app_manager.c    App 注册/打开/返回/屏幕销毁（资源释放）
│   ├── ui_launcher.c    桌面（网格来自 app_manager 注册表）
│   ├── ui_app_pages.c   各 App 页面（额度环/占位页）
│   └── ui_theme.c       深色主题 + 格式化工具
└── data/              ← 数据层（UI 的唯一数据入口 data_source.h）
    ├── data_source.c      前端：状态 + mock/bridge 分发
    ├── data_source_mock.c 伪随机 mock
    ├── data_source_bridge.c HTTP→Bridge /api/v1/quota/{id}/snapshot
    └── http_client.c      零依赖阻塞 HTTP GET（固件换 esp_http_client）

firmware/components/ui/    ← Phase 1+：CMake 引用同一份 src/ui + src/data
```

- 屏幕代码只依赖 LVGL + `data_source` 接口；同一份 UI 代码，mock 或
  Bridge 数据源由环境变量/配置切换，双端不改业务代码（ADR-001）。
- 显示驱动差异（SDL 窗口 vs CO5300 QSPI）只存在于各自的 main/port 层。
- App 屏幕"打开即建、退出即毁"（app_manager），对齐 PLAN §4.3
  单前台资源模型；Launcher 常驻。

## 4. Go Bridge 分层

```text
cmd/gadget-bridge/            main：配置加载、HTTP server 装配
internal/
  api/                        REST 路由（标准库 net/http，ADR-002）
  quota/                      额度域
    provider.go               QuotaProvider 接口 + QuotaSnapshot 模型
    registry.go               Provider 注册表
    store.go                  快照缓存（保留上次成功数据 + 过期标记）
    zhipu/                    智谱 Adapter（C 级接口）+ fixture 单测
    mock/                     Mock Provider（开发/测试）
  ai/                         Bigfish Chat 模型路由（后续）
  komari/ zcode/              监控数据聚合（后续）
  config/                     配置与密钥管理（环境变量/加密文件）
  logger/                     脱敏日志
```

额度域核心接口（与 PLAN §6.1 一致）：

```go
type QuotaProvider interface {
    ID() string
    DisplayName() string
    Capabilities() ProviderCapabilities
    FetchQuota(ctx context.Context, account AccountRef) (*QuotaSnapshot, error)
}
```

错误语义：

- 供应商接口异常 → 保留上次成功快照，标记 `stale`，UI 显示过期时间。
- 响应结构无法解析 → `unsupported_response` 错误，绝不显示伪造数据。
- `UsedPercent`（已用）与 `RemainingPct`（剩余）分开存，缺失保持 `nil`。

## 5. 数据流（首期三条）

### 5.1 订阅额度（以智谱为例）

```text
设备 Zhipu Quota App ──GET /api/v1/quota/zhipu/snapshot──▶ Bridge
Bridge（缓存命中且新鲜）──▶ 直接返回
Bridge（过期）──GET open.bigmodel.cn/api/monitor/usage/quota/limit──▶ 智谱
              └─ GET /api/biz/subscription/list（套餐，独立失败处理）
```

- 设备永不直连智谱；刷新周期默认 ≥5 分钟；预热仅用户手动触发。
- 两个上游接口独立请求独立容错，一个失败不影响另一个成功数据。

### 5.2 设备配网

```text
首次启动（NVS 无 Wi-Fi）→ SoftAP（随机密码）→ 屏幕二维码（只含 URL）
手机连热点 → http://192.168.4.1 → 扫描/选择 SSID + 密码 → STA 连接
成功 → 关 SoftAP → 屏幕显示局域网管理二维码
```

### 5.3 Bigfish Chat（后续）

```text
设备 Chat App ──POST /api/v1/chat/completions(SSE)──▶ Bridge Model Router
                                                      ├─▶ GLM/DeepSeek/豆包
设备录音 ──▶ Bridge ASR ──▶ completions ──▶ Bridge TTS ──▶ 设备播放
```

## 6. 当前落地状态

| 模块 | 状态 |
|------|------|
| Bridge：HTTP API + quota 域 + 智谱/Mock Adapter | ✅ Phase 0（fixture 单测 + 冒烟通过） |
| Bridge：Kimi Code Adapter（P1） | ✅ Phase 1（fixture 单测通过，默认关闭待真实账号验证） |
| Bridge：快照 SQLite 持久化（Phase 3） | ✅ 重启恢复实测通过（modernc.org/sqlite 纯 Go） |
| Bridge：凭据加密存储（Phase 3 收官） | ✅ AES-256-GCM 落库（DB 无明文实测）+ Web 账号配置页 + 保存热生效；`BRIDGE_MASTER_KEY` 缺失 fail-safe 禁用 |
| Bridge：WebSocket events（Phase 3） | ✅ `/api/v1/events` 推送 quota/komari 变更（2 组集成测试 + vite 代理 e2e） |
| Web：实时刷新 | ✅ Dashboard/Monitoring 订阅事件流（自动重连 + 实时徽标） |
| Bridge：Model Router（Phase 5） | ✅ `/api/v1/chat/completions` OpenAI 兼容透传 + SSE；env 配 upstream，无 Key 自动 mock |
| Web：聊天调试页 | ✅ /chat SSE 流式气泡 |
| 模拟器：大肥鱼聊天 App | ✅ 气泡 UI + pthread 异步 + **SSE 流式**（chunked 解析 e2e 46 chunks） |
| Voice Session 骨架（Phase 5） | ✅ 状态机/独占租约/音频 ops 抽象（LVGL 无关）+ audio_mock + Muse 位演示页；固件 real adapter 同签名待真机 |
| 语音链路接 Bridge（Phase 5） | ✅ `POST /voice/interact` PCM→WAV+双文本头；**LLM 已接 Model Router**（口语化 prompt + 失败回退）；ASR/TTS stub 待真实引擎 |
| 模拟器：LVGL 9 + SDL2 + 480×480 | ✅ Phase 1 骨干：Launcher + app_manager（注册表/生命周期/资源释放）+ 额度页 |
| 模拟器数据层：data_source（mock/bridge 可切） | ✅ Phase 1：bridge 模式实测命中 Bridge API（首刷 1 次请求后缓存） |
| 模拟器：中文 UI + CJK 字体子集 | ✅ Noto Sans SC（OFL）4 字号，`scripts/gen_cn_font.sh` 可重生成 |
| Web：Vue3+TS+Vite+Tailwind | ✅ Phase 2 骨干：Dashboard/Network(Wi-Fi+代理)/配网向导（设备 API 为 Mock）+ Token 认证 |
| Bridge：API Token 认证（PLAN §19） | ✅ `BRIDGE_TOKEN` 启用 Bearer 认证（401/200 实测） |
| Bridge：Komari 聚合器（Phase 4） | ✅ Client+Mock+30s 缓存，`/api/v1/komari/nodes`；真实接口 C 级待部署验证 |
| Bridge：智谱预热（Phase 4） | ✅ 幂等/冷却/周额度保护/前后对比，`POST /api/v1/quota/zhipu/prewarm`（6 组单测） |
| Web：Monitoring 监控页 | ✅ 服务器卡片 + 资源条 + stale 警示 |
| Web/模拟器：预热交互 | ✅ 二次确认 + before→after 对比 + 免责说明（模拟器含确认弹窗） |
| 模拟器：服务器监控页 | ✅ 替换占位；data_source 拉双额度+服务器（e2e 各恰 1 请求） |
| Firmware：ESP-IDF v6.0.3 | ✅ 首建通过（ai_gadget.bin ≈0.6MB，未烧录真机） |
| 语音/配网/真机驱动 | ⏳ 未开始（见 PLAN Phase 5/6） |
