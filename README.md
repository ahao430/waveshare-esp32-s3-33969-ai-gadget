# Waveshare ESP32-S3 33969 AI Gadget

基于 [Waveshare ESP32-S3-Touch-AMOLED-2.16（SKU 33969）](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm)
的社区定制多应用 AI Gadget。非 Waveshare 官方固件。

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
├── bridge/            # Go Bridge：额度聚合、AstrBot 机器人、天气、Komari、
│   │                  #   按键映射、桌面排布、外观、凭据（AES-256-GCM 落库）
├── web/               # Vue 3 管理后台（11 个配置页，手机/PC 响应式）
├── simulator/lvgl-pc/ # LVGL PC 模拟器（480×480，主 UI 开发环境，含中文字体子集）
│   └── assets/icons/  # 桌面应用图标（自备素材 + 缺位回退）
├── assets/            # 设计素材（概念图、吉祥物，非官方资产）
├── docs/              # 硬件、架构、决策、开发、bring-up 文档
├── scripts/           # check.sh 全仓检查 / gen_cn_font.sh 字体子集 等
└── PLAN.md            # 项目总规划
```

## 快速开始

### 全仓检查（提交前必跑）

```bash
scripts/check.sh    # bridge go vet/build/test + web typecheck + simulator cmake + firmware idf.py build
```

### LVGL PC 模拟器（UI 主开发环境）

```bash
brew install sdl2           # macOS 依赖（首次）
cmake -S simulator/lvgl-pc -B simulator/lvgl-pc/build
cmake --build simulator/lvgl-pc/build -j
./simulator/lvgl-pc/build/gadget_sim            # mock 数据源
GADGET_DS=bridge GADGET_BRIDGE=http://127.0.0.1:8081 \
    ./simulator/lvgl-pc/build/gadget_sim        # 连真 Bridge
# headless 回归钩子：GADGET_SNAP / DEMO_APP / DEMO_PAGE / DEMO_KEYS
```

### Go Bridge + Web 管理后台

```bash
cd bridge && ./run.sh     # :8081（Mock Provider 起步；凭据存 SQLite 加密）
cd web && npm run dev     # :5173
```

### 固件（需本机安装 ESP-IDF v6.0.x，环境搭建见 docs/DEVELOPMENT.md）

```bash
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor   # 真机刷写（先备份原厂固件！）
# Wi-Fi 凭据：cp main/wifi_secret.h.example main/wifi_secret.h（已 gitignore）
# lvgl 组件下载卡死时：scripts/fetch_lvgl_component.sh 预置后重试
```

## 状态（2026-11-15）

| 模块 | 状态 |
|------|------|
| 真机 bring-up（Phase 6 前半） | ✅ 全外设验证通过：I2C×6 / 显示 / 触摸 / 音频双向 / TF / RTC / AXP2101 真读数 / Wi-Fi→Bridge / 充电屏保 |
| 模拟器 UI | ✅ Launcher（多页/页点/图标）+ AstrBot 对话 / 小智 / 天气（Open-Meteo）/ 额度 / 监控 / 订阅 / 外观 / 按键映射（Web 下发端到端验证） |
| Bridge | ✅ 额度域 + 凭据 + 订阅 + bots(AstrBot config_id/provider/model) + 天气 + komari + 按键/排布 + 外观 + events(WS) + 语音端点 |
| Web 后台 | ✅ 11 页：仪表盘/订阅/额度/凭据/机器人/外观/按键/天气/排布/监控/ZCode |
| 固件 UI 移植（Phase 6 后半） | 🚧 下一阶段：模拟器 UI 上真机（局部重绘 + 节流约束） |
| 设备端 ZCode / 音乐 / 小智协议 | ⏳ 规划中 |

里程碑与详细进度见 [docs/PROGRESS.md](docs/PROGRESS.md)。
所有未在真机验证的内容都在 docs 中以 TODO 标注，不声称已测试。

## 安全原则

- API Key / Cookie / OAuth Token 永不进入固件、日志、Git 和二维码。
- 供应商凭据与 AstrBot token 只保存在 Go Bridge（AES-256-GCM 落库、不回显）。
- 供应商订阅额度接口按可信度分级（A/B/C/D），未公开接口默认关闭。
- 原厂固件闭源且不可找回——**刷写前必须全片备份**（见 BRINGUP.md §1.0）。
