# Companion 角色动画资源规范与制作管线

> 依据 ADR-005（三 AI App + 共用 Companion Engine）与用户提供的
> companion-sequence-assets-v1 素材包（概念图集）制定。
> **当前状态**：v1 图集为概念素材（3 帧/动作，帧未对齐、含装饰），
> **不可批量切图直接播放**；本文件定义最终资源目录规范与制作管线。

## 1. 统一动作状态接口（三角色共用）

| ID | 中文 | 播放方式 | 触发来源 |
|----|------|----------|----------|
| `idle` | 待机 | 循环 | 会话空闲（默认） |
| `listening` | 聆听 | 循环 | voice_session LISTENING |
| `thinking` | 思考 | 循环 | voice_session THINKING |
| `speaking` | 说话 | 循环 | voice_session SPEAKING |
| `happy` | 开心 | **播放一次** | 回复完成/互动成功 |
| `surprised` | 惊讶 | **播放一次** | 特殊回复/事件提醒 |
| `wave` | 挥手 | **播放一次** | App 启动/打招呼 |
| `sleep` | 睡觉 | 循环 | 屏幕休眠（PLAN §13） |

- 与 voice_session 的映射：LISTENING/THINKING/SPEAKING 一一对应；
  idle 默认；`happy` 在 Speaking 结束后触发一次；`surprised`/`wave`/
  `sleep` 由系统事件（通知/启动/息屏）触发。
- 三角色共用接口、各自独立造型与动作资源；扩展动作（跳舞/哭泣/
  疑惑/愤怒/节日）在接口上追加 ID，不动引擎。

## 2. 设备资源目录规范

```text
companions/
├── muse/
│   ├── idle/      frame_00.bin .. frame_NN.bin   # RGB565A 帧文件
│   ├── listening/ ...
│   ├── thinking/  ...
│   ├── speaking/  ...
│   ├── happy/     ...
│   ├── surprised/ ...
│   ├── wave/      ...
│   └── sleep/     ...
├── bigfish/
│   └── （同结构）
├── xiaozhi/
│   └── （同结构）
└── manifest.json                                 # 帧数/时长/画布元数据
```

- 画布：**固定 240×240**（或 200×200），透明背景（RGB565A：1bit alpha）
- 帧率：8–12 fps；每动作 6–8 帧
- 命名：`frame_NN.bin`（零填充两位）；`manifest.json` 记录每动作
  `{frames, fps, loop|once, canvas_w, canvas_h}`
- Flash 放置：UI 资源分区（PLAN §12.1"UI 必要资源"），设备经
  companion_engine 按状态加载播放

## 3. 制作管线（图集 → 设备资源）

```text
v1 概念图集（concept 图）
  │ ① 逐动作独立制作连续帧（6–8 帧/动作，透明背景、固定画布、
  │    帧间姿态连续）—— 美术阶段（手工/AI 辅助逐帧）
  ▼
连续帧 PNG 序列（frame_NN.png, RGBA）
  │ ② 校验脚本 scripts/companion_check.py：
  │    尺寸一致/帧数达标/alpha 非全 0/命名规范
  │ ③ 转换脚本 scripts/companion_convert.py：
  │    RGBA8888 → RGB565A（RGB565 + 1bit alpha）
  │    输出 frame_NN.bin + 更新 manifest.json
  ▼
companions/<char>/<action>/frame_NN.bin + manifest.json
  │ ④ 烧录进 UI 资源分区 / 拷入模拟器 assets
  ▼
companion_engine 按 manifest 播放（loop/once 回 idle）
```

- ②③ 两个脚本属于工程侧（可自动化），① 是美术侧（人工/AI 逐帧）。
- ⑤ 真机验证：AMOLED 实际显示帧率/内存（每帧 240×240×3B ≈ 170KB，
  池/PSRAM 策略按 PLAN §14 评估）。

## 4. Companion Engine 接入（代码侧现状与计划）

- 现状：`companion.c` 按 App 资源映射（dsh gif/小智图/吉祥物），
  状态切换接口已就位（`companion_set_state`）。
- 计划：资源表扩展为 8 态 × 3 角色（manifest 驱动）；播放方式
  loop/once（once 播完回 idle）；`sleep` 态由 power_manager 息屏
  事件触发；`wave` 在 App 启动时触发一次。
- 模拟器先行：逐帧资源就位前，模拟器继续用现有 GIF/静态形象。

## 5. 来源

- v1 概念图集：用户提供的 companion-sequence-assets-v1.zip
  （`assets/companion-v1/`，含 manifest.json 状态清单与三角色
  概念图集）。
