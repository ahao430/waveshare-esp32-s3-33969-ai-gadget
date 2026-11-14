# AGENTS.md — AI 协作者工作手册

给在本仓库工作的 AI 编码代理（ZCode/Claude Code 等）的操作手册。
人类开发者同样适用。目标：不读完全部 PLAN.md 也能安全高效地干活。

## 项目一句话

Waveshare ESP32-S3 圆屏 AI Gadget 的完整自定义固件栈：
**模拟器先行开发 UI（LVGL/C）→ Go Bridge（凭据/额度/AI 路由/配置）→
Vue3 Web 后台 → ESP-IDF v6 固件**。四件套共享一份契约，模拟器与真机
共用 UI 源码（ADR-001，移植进行中）。

## 仓库地图（改哪里找哪里）

| 要改什么 | 位置 | 备注 |
|----------|------|------|
| 设备 UI（Launcher/App 页面） | `simulator/lvgl-pc/src/ui/` | **模拟器与真机共用源**；新增 .c 要手改 ui CMakeLists（两处） |
| UI 公共件（页点/手势） | `simulator/lvgl-pc/src/ui/ui_gesture.*` | 页点定位用 `ui_page_of_scroll`（实测宽+四舍五入，勿硬编码 480） |
| 数据源抽象 | `simulator/lvgl-pc/src/data_source.*` | mock / bridge 两实现；新字段三处同步（.h/两 .c） |
| Bridge API | `bridge/internal/api/` | 标准库 net/http，Go 1.22+ 路由模式 `GET /api/v1/...` |
| Bridge 配置存储 | `bridge/internal/storage/` | bots/app_settings 表；密钥字段 Encrypt 后存 JSON |
| AI 上游（AstrBot/OpenAI 兼容） | `bridge/internal/ai/` | astrbot payload 契约见 astrbot.go 头注释 |
| Web 页面 | `web/src/pages/*.vue` | 加页要同步 router/index.ts + App.vue nav + Dashboard 入口 |
| 固件 | `firmware/main/`（bring-up 骨架）、`firmware/components/power_manager/` | UI 移植阶段会把 simulator 源编进来（main/CMakeLists 预留注释） |
| 中文字体 | `scripts/gen_cn_font.sh` → `simulator/lvgl-pc/src/ui/fonts/font_cn_*.c` | 改字典/加字后必须重新生成，否则真机/模拟器豆腐块 |
| 桌面图标 | `simulator/lvgl-pc/assets/icons/`（88×88） | 缺位自动回退符号；名字与 launcher catalog key 对齐 |

## 提交前必跑

```bash
scripts/check.sh   # bridge: go vet+build+test / web: typecheck / simulator: cmake / firmware: idf.py build
```

全绿才提交。只改 docs 可以豁免。

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
1. **`bsp_display_lock(0)` = 无限等待**（非非阻塞！）。有限超时 take 超时
   会触发 FreeRTOS 递归互斥量断言崩溃——跨任务 UI 操作一律 `lock(0)` + 成对 unlock
2. **全屏高频重绘会堵死显示管线**（白屏/LVGL 挂起，断电才能恢复）——
   设备 UI 只做局部失效 + 帧率节流；全屏刷新仅限页面切换
3. `esp_timer` 周期单位是 **µs**（按 ms 传会快 1000 倍）
4. KEY3（GPIO18）机械抖动需 200ms 去抖
5. 新增头文件后 ninja 依赖不回溯——`touch main.c` 强制重编
6. GOT_IP 等事件回调里更新 UI：必须持显示锁；Wi-Fi 启动要在 UI 构建之后

### LVGL 版本差异（移植期主要摩擦）
- 模拟器 LVGL 9.2.2 / 固件更新：`lv_obj_add_flag/clear_flag` 在固件是
  deprecated-error，改用 `lv_obj_set_hidden/clickable` 等专用 setter
- 触摸单点（无手势库），交互按单点设计

## 验证方式（按信任级）

1. `scripts/check.sh` 全绿——编译级
2. Bridge：`go test ./...`（api/storage 层有契约测试）
3. 模拟器像素验证：`GADGET_SNAP=/path.ppm` 截帧 + PPM 直读/差分脚本
   （页点位置、颜色值、标签 span 都可断言）；headless 钩子
   `DEMO_APP=<key>` / `DEMO_PAGE=<n>` / `DEMO_KEYS="pw@2000,key3long@6000"`
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

- Bridge `:8081`（master key 在 /tmp/bridge.log 启动行；日志同处）
- Web `:5173`；模拟器 GUI 可常开（bridge 模式实时连）
