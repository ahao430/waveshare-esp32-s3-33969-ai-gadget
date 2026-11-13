# 进度对照表（PLAN §18 验收标准逐项核对）

> 最后更新：2026-11-14。状态标记：✅ 完成 · 🔶 部分完成 · ⏳ 未开始 ·
> 🔒 等外部输入（解锁条件注明）。证据见 [DEVELOPMENT.md](DEVELOPMENT.md)。

## Phase 0：项目初始化 ✅

| 验收项 | 状态 | 说明 |
|--------|------|------|
| 阅读 Waveshare 官方仓库/Wiki/BSP | ✅ | HARDWARE.md（BSP 源码交叉确认引脚表） |
| 确认 BSP 版本 | ✅ | `waveshare/esp32_s3_touch_amoled_2_16 ^2.0.1` |
| 确认 ESP-IDF 主版本 | ✅ | v6.0.x（ADR-004；实装 v6.0.3） |
| 检查 03_esp-brookesia 示例 | ✅ | 依赖极简（BSP 单依赖）；结论进 HARDWARE.md §3 |
| 创建 Monorepo | ✅ | 五目录结构（PLAN §17） |
| Firmware 最小工程 | ✅ | IDF v6.0.3 编译通过（0.6MB，85% 分区余量） |
| Bridge 最小工程 | ✅ | 9 个测试包全绿 |
| Web 最小工程 | ✅ | vue-tsc + vite build |
| 代码规范 | ✅ | 各工程风格约定（docs 沉淀） |
| CI 编译检查 | ✅ | scripts/check.sh 本地全绿 + GitHub Actions（.github/workflows） |

## Phase 1：PC UI ✅

| 验收项 | 状态 | 说明 |
|--------|------|------|
| LVGL PC Simulator 运行 | ✅ | LVGL 9.2.2 + SDL2，480×480 |
| 480×480 主题 | ✅ | 深色 OLED 友好（ui_theme.h = Web tailwind 同源色板） |
| Launcher | ✅ | 状态栏/网格/页面指示器/时钟 |
| App Manager | ✅ | 注册表/单前台/退出即毁（auto_del） |
| Mock 硬件层 | ✅ | data_source mock/bridge 双实现（ADR-001） |
| 页面跳转和返回 | ✅ | 转场动画；语音/聊天/监控页实装，其余占位 |
| 资源释放测试 | ✅ | on_exit 停定时器/租约；代码评审 + 运行验证 |
| （超额）CJK 字体 | ✅ | Noto Sans SC 子集管线（gen_cn_font.sh） |

## Phase 2：Web 管理后台 ✅

| 验收项 | 状态 | 说明 |
|--------|------|------|
| Vue3+TS | ✅ | 严格模式 typecheck |
| 手机/PC 响应式 | ✅ | <640px 顶部导航（§9.1）；真机浏览器待实测 |
| Dashboard | ✅ | 设备总览 + 额度卡 + 实时刷新 |
| Wi-Fi 页面 | ✅ | Mock 设备 API（接口签名按 §9.4 预定） |
| 代理配置 | ✅ | Direct/HTTP CONNECT（§10.2） |
| 设备设置 | 🔶 | API 已预留（config），UI 未做 |
| 二维码配网原型 | ✅ | 三步向导；二维码只含 URL（§9.3） |
| API Mock | ✅ | device.mock.ts |
| 认证与脱敏 | ✅ | Bridge Token（401/200 实测）；Key 不回显 |

## Phase 3：Bridge ✅（收官）

| 验收项 | 状态 | 说明 |
|--------|------|------|
| Go HTTP API | ✅ | 标准库 net/http（ADR-002） |
| WebSocket | ✅ | /api/v1/events；quota/komari 推送（2 组集成测试） |
| SQLite | ✅ | 快照持久化（重启恢复实测）+ 凭据表 |
| 配置加密 | ✅ | AES-256-GCM；DB 无明文实测；fail-safe 缺 key 禁用 |
| Provider 接口 | ✅ | quota.QuotaProvider（§6.1 契约） |
| 智谱 Adapter | ✅ | C 级；fixture 单测；🔒 真实账号验证待 Key |
| Mock Provider | ✅ | 伪随机确定性数据 |
| 额度快照和错误状态 | ✅ | stale/last_error/UnsupportedResponse 语义 |

## Phase 4：订阅与监控 App 🔶

| 验收项 | 状态 | 说明 |
|--------|------|------|
| 智谱额度 | ✅ | 模拟器+Web 双端显示；真实数据 🔒 待 Key |
| 手动预热 | ✅ | Bridge 状态机 6 测 + Web/模拟器交互 |
| Kimi Code | ✅ | Bridge Adapter（B 级）；模拟器页接双 provider |
| MiniMax | ⏳ | P2，PLAN 允许后置 |
| Komari | ✅ | Bridge 聚合 + Web/模拟器监控页；🔒 真实部署验证 |
| ZCode Remote | ⏳ | 🔒 数据源待拍板（本机 DB or 远程 API） |
| 验收标准（异常/无额度/重置/未知字段呈现） | ✅ | mock + fixture 覆盖四类场景 |

## Phase 5：AI Chat 与语音 🔶

| 验收项 | 状态 | 说明 |
|--------|------|------|
| Bigfish Chat 文字 | ✅ | Bridge Router + Web/模拟器双端 |
| 流式响应 | ✅ | SSE（Web fetch 流式 / 模拟器 chunked 解析，46-chunk e2e） |
| Audio Manager | ✅ | voice_session 状态机 + ops 抽象 + audio_mock |
| 语音采集 | ✅ 协议 | interact 一步式（PCM→WAV）；🔒 真机 I2S 待设备 |
| TTS 播放 | ✅ 协议 | WAV stub 生成；🔒 真机 ES8311 待设备 |
| Muse 独立验证 | ⏳ | 🔒 等设备 + Muse SDK 独立构建（材料齐：HARDWARE §3） |
| Xiaozhi 独立验证 | ⏳ | 🔒 协议文档+骨架设计齐（XIAOZHI_PROTOCOL.md）；等端点 |
| HTTP CONNECT 代理 | ⏳ | 设备侧网络栈随 Phase 6 |

## Phase 6：33969 真机适配 🔒 全部等设备

烧录/i2cdetect 已前置进骨架固件；逐项执行清单见
[BRINGUP.md](BRINGUP.md)（显示→触摸→音频→TF→PMU→RTC→IMU→按键→睡眠→Wi-Fi
+ Muse 坑 checklist 6 条 + 记录模板）。

## Phase 7：优化 ⏳

依赖 Phase 5/6 的真实链路；性能模板已备（BRINGUP.md §3）。

## 汇总

- **PC 侧可推进项：全部完成**。
- 等待点：真机设备、真实 Provider Key、Muse/小智服务端账号。
- 仓库存档：5 个逻辑提交（docs/bridge/simulator+firmware/web/chore）。
