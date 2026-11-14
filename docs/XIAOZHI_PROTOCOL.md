# 小智（xiaozhi-esp32）协议研究与 33969 适配设计

> 调研日期：2026-11-14。来源：上游仓库 `docs/websocket.md`、
> `docs/custom-board.md`（main 分支）。结论用于设计 `apps/xiaozhi`
> 的协议客户端与板级移植路径（PLAN §5.5 / ADR-003：独立 App + 共享
> Voice Session）。

## 1. 通信协议（WebSocket 路径）

### 1.1 通道建立

| 要素 | 值 |
|------|----|
| 触发 | 唤醒词或按键 `OpenAudioChannel()`（非全天候长连） |
| 鉴权头 | `Authorization: Bearer <token>`（服务端可拒绝握手） |
| 设备头 | `Protocol-Version`（=hello.version）、`Device-Id`（MAC）、`Client-Id`（UUID，擦 NVS 重置） |
| URL | settings 里配置，无 query 参数 |

### 1.2 hello 握手（双向 JSON）

设备 → 服务端：

```json
{
  "type": "hello",
  "version": <int>,
  "transport": "websocket",
  "features": {"mcp": true, "aec": true},
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

服务端 → 设备：必含 `type: "hello"` + `transport: "websocket"`（否则失败），
可带 `session_id` 与 `audio_params`。**10s 未收到有效 hello → 网络错误**。

### 1.3 帧：二进制 = Opus 音频，文本 = JSON

| Binary 版本 | 格式 |
|-------------|------|
| v1 | 裸 Opus 帧 |
| v2 | version/type/reserved/timestamp/payload_size/payload（timestamp 供服务端 AEC） |
| v3 | type/reserved/payload_size/payload 轻量头 |

- 上行 16kHz mono；下行可 24kHz（设备重采样播放）。
- Listening 状态下收到的音频帧**丢弃**。

### 1.4 消息类型

设备 → 服务端：

| type | 字段 | 用途 |
|------|------|------|
| `listen` | state: start/stop/detect; mode: auto/manual/realtime | 会话控制（PTT=manual） |
| `listen`(detect) | 可带 text + 唤醒词音频 | 本地唤醒词命中上报（声纹校验） |
| `abort` | reason（如 wake_word_detected） | 打断 |
| `mcp` | JSON-RPC 2.0 payload | 工具调用响应 |

服务端 → 设备：

| type | 字段 | 用途 |
|------|------|------|
| `stt` | text | ASR 结果（设备端显示"听到的话"） |
| `llm` | emotion, text | 表情更新 |
| `tts` | state: start/stop/sentence_start（带 text） | 播放控制 + 字幕 |
| `system` | command: reboot | 系统指令 |
| `alert` | status/message/emotion | 异常提示 |
| 无 type | — | 记日志忽略 |

### 1.5 IoT = MCP（JSON-RPC 2.0）

旧 `type: "iot"` 已弃用。服务端下发 `tools/call`（name + arguments），
设备回 `result.content[].text`。对 33969 的意义：亮度/音量/媒体控制
走 MCP 工具注册。

### 1.6 状态机与重连

`Starting → WifiConfiguring → Activating → Idle` ↔ 会话；
连接失败/hello 超时 → on_network_error；意外断开 → 回 Idle（可策略重试）。
激活/OTA 的消息格式在文档中未展开（TODO：读源码 `protocols/` 实现）。

## 2. 33969 板级移植路径

上游不使用 esp-bsp（原生 esp_lcd + 自有抽象），两种集成方式：

### 方案 A（PLAN 首选）：本仓库组件集成

把小智的**协议层与业务层**（protocols/、audio 编解码、状态机）作为
组件引入，板级按 PLAN `hardware/` 抽象适配：

| 小智概念 | 本仓库对应 |
|----------|-----------|
| `Board`/`WifiBoard` | `hardware/` real adapter（已有 BSP 装配） |
| `AudioCodec`（Es8311/BoxAudioCodec） | `audio_manager`（vs_audio_ops 同源；BSP 已封装 ES8311/ES7210 初始化） |
| `Display`（SpiLcdDisplay 等） | `display_manager`（BSP 的 CO5300 QSPI；上游无 QSPI AMOLED 现成 Display 类，需按 esp_lcd QSPI 写一个） |
| 唤醒词 ESP-SR | Phase 5 后续（PLAN 非必需） |
| `Axp2101`（common/） | 有现成类可参考（PMU 驱动） |

工作量集中点：**QSPI AMOLED 的 Display 适配**（上游只有 SPI/QSPI 小屏
驱动族，SH8601 QSPI 可参考接口形态）+ `AudioCodec` 到 BSP codec_dev
的桥接。

### 方案 B：上游加板（贡献给 xiaozhi-esp32）

按 `docs/custom-board.md`：`main/boards/waveshare-esp32-s3-amoled-2-16/`
（config.h 引脚 + board 类 + DECLARE_BOARD），Kconfig/`build.py` 注册。
优点：直接吃上游全部 App 逻辑；缺点：IDF 版本绑定上游（v6.0.1+，
我们基线一致 ✓）、与本项目系统服务（app_manager/voice_session）不共享
——两个"操作系统"并存的冲突。

**结论**：方案 A 为主线（协议客户端组件化），方案 B 仅在 A 失败或
需要完整上游功能时评估。语音硬件经 voice_session 租约接入，
`apps/xiaozhi` 实现 `ProtocolClient`（WS + opus）并在
`Thinking` 阶段等待服务端 tts start。

## 3. 客户端骨架映射（apps/xiaozhi 设计稿）

```text
apps/xiaozhi/
├── protocol_client.c   WS 连接/hello/消息分发（esp_websocket_client）
├── audio_pipe.c        采集 PCM → opus 编码 → binary 帧；下行解码 → 播放
├── xiaozhi_app.c       App 生命周期；vs_acquire("xiaozhi")；listen 手动模式
└── mcp_tools.c         亮度/音量等 MCP 工具注册（JSON-RPC 响应）
```

复用：voice_session（PTT/状态机映射——Listening=listen start、
Thinking=等 stt+tts start、Speaking=tts sentence_start→stop）、
audio_manager（codec_dev）、display_manager。

协议与 PLAN 语音栈的对应关系已闭合；实现顺序：先 WS+JSON 控制帧
（无音频可打通状态机），再 opus 管线。

## 4. 音色与角色提示词的实现路径（设计结论，2026-11-14）

小智的音色选择与角色提示词由**服务端会话配置**管理（xiaozhi.me console
设备详情页），设备端不持有这些参数。我们的后台（Web）要设置时有两条路径：

| 路径 | 原理 | 可行性 |
|------|------|--------|
| A. 官方服务 | Bridge 代理调用 xiaozhi.me 的 console 内部 API（登录 token + 设备配置接口） | C 级接口：无公开 API 承诺、需用户授权 token；按 PLAN §6.2 规则做成"可选启用 + 明确标注实验性"，不承诺稳定 |
| B. 自建服务端 | 小智支持自建 server（xiaozhi-esp32 服务端组件）；音色/提示词存自建 DB | **推荐**：Bridge/Web 直接读写自建 server 的配置（我们的数据库我们的 API），设备经 WS 连自建端点 |

结论：
- 用户用官方服务 → 我们的后台只做**跳转**（设置页已加
  xiaozhi.me/console 全屏码）+ 提示"音色/提示词在官方后台配置"。
- 用户自建 server → Bridge 加 `xiaozhi/` 域对接自建 server 配置 API，
  Web 后台提供音色列表下拉 + 角色提示词编辑（数据模型：
  per-device {voice_id, system_prompt}，下发经 server 会话建立时生效）。
- 混合形态（官方账号 + 想在设备端快速切音色）→ 设备端 App 内不实现
  音色切换（协议不支持设备侧设置），保持"后台管理"边界（PLAN §1.1）。

设备端 App（apps/xiaozhi）的边界不变：协议客户端只管
hello/listen/tts 帧与会话状态机，音色/提示词是服务端会话属性。
