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
> 出厂系统架构审查见 [docs/FACTORY_REVIEW.md](docs/FACTORY_REVIEW.md)，
> 真机到手后按 [docs/BRINGUP.md](docs/BRINGUP.md) 执行 Phase 6。

## 仓库结构

```text
waveshare-esp32-s3-33969-ai-gadget/
├── firmware/          # ESP32-S3 固件（ESP-IDF + LVGL）
├── bridge/            # Go Bridge：云端 API、订阅额度、凭据管理
├── web/               # Vue 3 管理后台（手机/PC 响应式）
├── simulator/lvgl-pc/ # LVGL PC 模拟器（480×480，主 UI 开发环境）
├── assets/            # 设计素材（概念图、吉祥物，非官方资产）
├── docs/              # 硬件、架构、决策、开发文档
├── scripts/           # 构建/检查脚本
└── PLAN.md            # 项目总规划
```

## 快速开始

### LVGL PC 模拟器（UI 主开发环境）

```bash
brew install sdl2           # macOS 依赖（首次）
cmake -S simulator/lvgl-pc -B simulator/lvgl-pc/build
cmake --build simulator/lvgl-pc/build -j
./simulator/lvgl-pc/build/gadget_sim
```

### Go Bridge

```bash
cd bridge
go build ./...
go test ./...
./run.sh                   # 启动开发服务器（Mock Provider）
```

### Web 管理后台

```bash
cd web
npm install
npm run dev
```

### 固件（需本机安装 ESP-IDF v6.0.x，环境搭建见 docs/DEVELOPMENT.md）

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
# lvgl 组件下载卡死时：scripts/fetch_lvgl_component.sh 预置后重试
```

## 状态

Phase 0（项目初始化）进行中。当前里程碑见
[PLAN.md 第 18 节](PLAN.md)。所有未在真机验证的内容都在
docs 中以 TODO 标注，不声称已测试。

## 安全原则

- API Key / Cookie / OAuth Token 永不进入固件、日志、Git 和二维码。
- 供应商凭据只保存在 Go Bridge。
- 供应商订阅额度接口按可信度分级（A/B/C/D），未公开接口默认关闭。
