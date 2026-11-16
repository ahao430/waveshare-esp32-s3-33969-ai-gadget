# 小智设备直连官方服务（ADR-014 实施设计）

> 目标：小智 App 绕过 Bridge 直连 xiaozhi.me 官方——音频链从
> 设备↔Bridge↔官方 变为 设备↔官方，消除 Bridge 部署位置引入的绕路延迟。
> 协议参考 = Bridge 的 Go 实现（internal/xiaozhi/，ADR-005 已真网验证）。
>
> **状态（2026-11-17）：暂停（ADR-015）。** 已跑通：OTA+激活
> （bound=1）、WSS 握手、Opus 编码上行。未收敛：PTT 后概率性内核数据
> 损坏（与内存压力相关，排查被坑 #14 实验变量污染）。源码存档
> firmware/main/xz_*.c/h（未编译；恢复接线 = CMakeLists SRCS 加回
> xz_*.c + idf_component.yml 加回 esp_audio_codec 2.6.2 +
> sdkconfig EXTERNAL_MEM_ALLOC/IN_CONTENT_LEN=4096 + ui_conversation
> 的 xz 分支，diff 见 git 历史）。
>
> **证据索引（重启项目先读）**：
> - mbedtls -0x008D = PSA_ERROR_INSUFFICIENT_MEMORY（-141）≠加密损坏；
>   EXTERNAL_MEM_ALLOC + IN_CONTENT_LEN=4096 下 OTA 真网通过
> - esp_websocket_client 任务栈 4KB 内部 RAM 固定 → 对话页建不出来；
>   自实现同步 WSS（esp_tls+RFC6455）跑在静态任务可行（xz_session.c）
> - esp_audio_codec 2.x 需显式 esp_opus_enc/dec_register()（xz_opus.c）
> - 崩溃形态：xSuspendedTaskList.pxIndex=NULL / SPI 总线锁坏 /
>   渲染线程 __stack_chk_fail；均在 enc open 后首帧窗口
> - 内存画像见 AGENTS「内存/flash 账本」：对话页稳态内部最大块 7.7KB
>   是结构性约束（官方板无此约束）

## 1. 协议（从 Go 实现提取，逐字段对齐）

### OTA 注册（HTTPS POST `https://api.tenclass.net/xiaozhi/ota/`）

头：
```
Device-Id: 02:xx:xx:xx:xx:xx（本地管理位 MAC，稳定身份）
Client-Id: <uuid v4>
User-Agent: gadget-bridge/0.1.0（设备端改为板型名）
Activation-Version: 1
Accept-Language: zh-CN
Content-Type: application/json
```
体（系统信息）：version=2 / language=zh-CN / mac_address / uuid /
chip_info{model:9(esp32s3), cores:2, revision:2} / application{name,version,
idf_version,compile_time} / board{type,name,mac,ssid,rssi,channel} /
protocol.features **全部 false**（不实现 MCP/UDP/AEC，别 advertise）。

响应：`websocket{url,token}` + `activation{code,message,challenge}`（未绑定时）
+ `server_time`。**无论绑定与否都下发 ws url/token。**

### 激活查询（POST `<ota>/activate`，body `{}`，同头）

- 202 = 等待用户在官方控制台输入激活码
- 200 = 已绑定 → 绑定完成需 **re-OTA** 刷新 token（绑定前后可能不同）

### WebSocket 会话（wss，官方 url）

握手头：`Authorization: Bearer <token>` / `Protocol-Version: 1` / `Device-Id` / `Client-Id`

回合流程（PTT manual 模式，同 Bridge AskVoice）：
1. 发 `{"type":"hello","version":1,"transport":"websocket","features":{全 false},
   "audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}`
2. 收服务端 hello（记 `session_id`；transport 必须 websocket）
3. 发 `{"type":"listen","state":"start","mode":"manual","session_id":…}`
4. 上行：二进制 WS 消息 = **裸 Opus 包**（v1，一包一帧，960 样本@16k=60ms；
   VBR 长度不定，WS 消息边界即包边界；尾帧不足补静音）
5. 发 `{"type":"listen","state":"stop","session_id":…}`
6. 下行事件：`stt{text}`（转写）/ `llm{emotion}` / `tt­s{state:sentence_start|stop,text}`
   （字幕增量/回合结束）/ 二进制 = Opus TTS 音频（官方常 24k 编码，解码器
   按 16k 输出即自动重采样）/ `alert{text}`（错误提示）
7. `tts stop` = 回合结束；20s WS ping keepalive；单回合 60s 空闲上限

## 2. 固件架构

```
firmware/main/xiaozhi_direct/   （新模块，ESP-only，不进模拟器）
  xz_identity.{c,h}  NVS 身份：device_id/client_id/ws_url/ws_token/bound
                      （首次生成 MAC+UUID，OTA 结果写回；namespace "xiaozhi"）
  xz_ota.{c,h}       HTTPS OTA/activate（esp_http_client + esp_crt_bundle）
  xz_opus.{c,h}      Opus 编解码封装（16k/mono/60ms；编解码器状态 PSRAM）
  xz_session.{c,h}   WSS 回合（esp_websocket_client；事件→回调→PCM 直灌
                      audio_esp32_tts_* 流式播放接口）
```

- **组件依赖**：`espressif/esp_websocket_client`（WSS）+ Opus 实现
  （espressif/esp_audio_codec 或 libopus 移植，实测后定）+ IDF 自带
  esp_crt_bundle
- **内存**：Opus 编/解码器状态各 ~30-40KB → `heap_caps_malloc(MALLOC_CAP_SPIRAM)`；
  TLS 走 `MBEDTLS_EXTERNAL_MEM_ALLOC`（握手缓冲不占内部 RAM）
- **flash**：ADR-013 后余 8.36MB，无压力

## 3. 交互与 UX

- **PTT 不变**：KEY3 长按说话 → 松开发起回合（官方 manual listen 模式，
  与 Bridge 版完全同 UX）；录音 PCM 缓冲复用现有语音回合静态栈任务
- **激活**：小智 App 未绑定时页面大字显示 6 位激活码 + 提示
  "到 xiaozhi.me 控制台输入"（3s 轮询 activate，绑定成功自动 re-OTA 并刷新 UI）
- **回退**：直连不可用（未激活/网络失败/token 过期）自动回落现有
  Bridge 链（设备协议零改动）；设置页不动，小智 App 内部自动选择
- **人设/音色**：绑定后走官方控制台配置（同官方设备体验）

## 4. 实施顺序

1. 组件引入 + 空壳编译（量 flash 增量、验证 CDN 可达）——风险前置
2. xz_identity + xz_ota（真机跑通 OTA 拿激活码，屏幕显示）
3. 激活轮询 + re-OTA（绑定闭环）
4. xz_opus + xz_session（WSS 回合，PCAP 级对齐 Go 行为）
5. 接入 ui_conversation 小智 App（direct 优先/bridge 回退）
6. 真机端到端 + 延迟对比（直连 vs Bridge 中转）
