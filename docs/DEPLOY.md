# Bridge 部署指南（脱离 Mac，ADR-009）

目标：Bridge + Web 后台跑在一台**常驻机器**（Linux 服务器/NAS/树莓派），
设备经运行期配网指向它；Mac 只用于开发。

## 0. 为什么不能塞进设备

ESP32-S3 跑不了 Go，且项目安全模型是「API Key 只进 Bridge
（AES-256-GCM 落库）」，几十个供应商 Key 不能存到设备上。Bridge 是
零第三方依赖的静态二进制 + SQLite，部署成本极低。

## 0.5 AstrBot 也要自部署（聊天与语音回复的后端）

Bridge 不自带对话模型——它把聊天转发给 **AstrBot**（开源 GPT 风格机器人平台）。
AstrBot 需另行部署（Docker 或源码，任意常驻机器，可与 Bridge 同机）：

1. 部署 AstrBot 并配置模型供应商（如 DeepSeek/智谱官方 API）。
2. 在 AstrBot 侧准备 API Token（Web 后台/开放平台设置里生成）。
3. Bridge Web「AstrBot」页填服务地址 + Token → 保存 →「测试」应回 pong。
4. 之后在「AstrBot」页建机器人实例（对应 AstrBot 的配置文件/人设），设备端可选实例。
5. 语音识别 Key（硅基流动/豆包/DeepSeek 等）在「语音设置」页配；语音回复文本同样走 AstrBot。

## 1. 构建产物

```bash
scripts/build-release.sh
# dist/webdist/            Web 后台静态文件（Bridge 直接托管）
# dist/gadget-bridge-*     本机二进制；linux 交叉（需 musl-cross，可跳过）
# dist/gadget-bridge-*.tar.gz
```

## 2. 目标机构建（推荐，等价于交叉编译）

目标机需要 go ≥1.22 与 gcc（cgo libopus）；node 仅当前端不在本机构建时需要。

```bash
git clone <repo> && cd waveshare-esp32-s3-33969-ai-gadget
# 前端：本地构建后把 web/dist 传上来（或目标机装 node 跑 npm run build）
tar -xzf gadget-bridge-*.tar.gz -C /opt/gadget   # 解出 webdist/
(cd web && npm install && npm run build && cp -R dist /opt/gadget/webdist)
cd bridge && go build -tags nolibopusfile -o /opt/gadget/gadget-bridge ./cmd/gadget-bridge
```

## 3. 数据迁移（从 Mac）

**BRIDGE_MASTER_KEY 必须原样拷贝**——凭据用它加密，换 key 全部读不出：

```bash
scp bridge/data/gadget.db  user@server:/opt/gadget/data/gadget.db
# bridge.env（含 BRIDGE_MASTER_KEY）手工在服务器重建：
cat > /opt/gadget/bridge.env <<'EOF'
export BRIDGE_MASTER_KEY=<与 Mac 相同的 64 位 hex>
# ⚠️ 设备接入时不要设 BRIDGE_TOKEN：设备 HTTP 请求不带 Authorization，
# 设了 token 设备全部接口 401（见 §5「设备侧的协议限制」）。
# export BRIDGE_TOKEN=<可选：仅供纯浏览器/局域网访问；设备要用就不能设>
EOF
```

小智/Muse/AstrBot/订阅的加密凭据都在 gadget.db 里，迁移后原样可用。

## 4. 运行（systemd）

```ini
# /etc/systemd/system/gadget-bridge.service
[Unit]
Description=Gadget Bridge
After=network-online.target

[Service]
WorkingDirectory=/opt/gadget
ExecStart=/opt/gadget/gadget-bridge -addr :8081 -webdir /opt/gadget/webdist
EnvironmentFile=/opt/gadget/bridge.env
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now gadget-bridge
curl http://127.0.0.1:8081/api/v1/device/status   # 验证
```

- `-webdir` 让 Bridge 托管 Web 后台：手机访问 `http://<服务器>:8081`
  即后台（SPA，改配置即时生效），设备「设置→管理后台」二维码就是这个地址。
- 防火墙放行 8081（或反代到 80）。**反向代理不要给设备链路做
  https-only / http→https 强跳**：设备只支持明文 `http://`，跳转会直接连不上
  （Cloudflare 橙云只能代理 80/443，8081 需灰云或直连；且要关
  「Always Use HTTPS」）。
- 公网暴露时**不要设 `BRIDGE_TOKEN`**（设备不发 Authorization，设了 401）。
  接口无鉴权时用非默认端口 + 防火墙/IP 白名单收窄暴露面（详见 §5）。
- 语音识别（Edge-TTS/DeepSeek ASR/小智官方）都从服务器出网，注意
  服务器网络可达性（需要代理的环境在 bridge.env 加 `proxy_url` 相关
  变量，见 README §代理）。

## 5. 设备侧（运行期配网）

设置 → 网络与连接：

1. **Wi-Fi 设置**：扫描 → 选网 → 软键盘输密码 → 连接（失败可改密重试；
   成功写入 NVS，重启记住）。
2. **Bridge 地址 = 地址簿**：可保存多个地址、点一下切换（✓=当前）：
   - 开发调试：本机 `http://192.168.x.x:8081`（加上会随 Bridge 一起启动的 Web 后台）
   - 正式使用：服务器 `http://<域名或IP>:8081`
   切换即生效（对话/订阅立刻走新地址，无需重刷）；非当前地址可删除，
   当前地址不可删。开发机和服务器各部署一套 Bridge 时，用同一个
   `BRIDGE_MASTER_KEY` 可让配置库互相拷贝通用。
3. **预置地址（可选，免软键盘输入）**：`wifi_secret.h` 里
   `BRIDGE_SEED_HOSTS "http://a:8081,http://b"`（逗号分隔）——开机把每条
   幂等并进地址簿（不改当前激活地址），适合公网域名这种长地址。

### ⚠️ 设备侧的协议限制（公网部署必读）

- **只支持明文 `http://`**：固件 HTTP 客户端没有 TLS（`http_client.c`
  只认 `http://`），所以公网 Bridge 必须开 **80 或 8081 明文端口**。
  只开 443/HTTPS（如 Caddy 默认的 http→https 重定向）设备连不上；
  需要给设备单独留一个明文入口。
- **不能设 `BRIDGE_TOKEN`**：设备请求不带 `Authorization`（本次未实现），
  一旦服务器设了 token，设备全部接口 401。代价是公网明文接口无鉴权——
  请用防火墙/IP 白名单/非默认端口收窄暴露面，或后续给设备补 token 支持。
- **设备所在 Wi-Fi 必须允许出站 TCP**：公司访客网常做完全隔离——实测
  GYJX-GUEST 对服务器 80/443 和局域网 Mac 8081 全部 TCP 超时（DNS/DHCP
  可用）。串口启动日志 `net probe: status=-4 ... stage=3` 就是这种情况
  （3=select 超时），换到能出网的 Wi-Fi/热点即可，与 Bridge 无关。
- 设备会把 Bridge 地址存进 NVS；**换服务器地址不需要重刷固件**，
  在设备地址簿点选即可（预置只是省输入）。

凭据优先级：NVS（配网页保存）> `firmware/main/wifi_secret.h`（编译期
兜底，刷开发固件时仍可用）。

## 6. 日常

- Web 改配置（机器人/音色/订阅/排布）即时生效，设备不用动。
- Bridge 升级：替换二进制 + `systemctl restart gadget-bridge`
  （master key 不变，凭据无感）。
- 备份：定时拷 `data/gadget.db` 即可（全部状态都在这一个文件）。
