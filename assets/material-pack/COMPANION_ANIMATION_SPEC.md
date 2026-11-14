# 智能助手动态伙伴设计规格

## 目标

Muse 和 Bigfish（默认大肥鱼角色）在各自聊天页面中作为动态伙伴出现。Bigfish 内可创建多个助手 Profile，角色形象与模型 Provider 解耦。动作必须与真实交互状态同步；不播放无语义的长循环，不让角色在等待模型时误表现为正在说话。

## 状态机

| 状态 | 进入条件 | 退出条件 | 动作方向 |
|---|---|---|---|
| `BOOT` | 打开 App / 初始化完成 | 进入 IDLE 或恢复会话 | 登场、短挥手 |
| `IDLE` | 无活动会话 | 用户触摸/开始录音/发消息 | 呼吸、眨眼、轻微待机 |
| `LISTENING` | 麦克风采集开始 | 采集停止或提交语音 | 专注聆听、身体前倾 |
| `THINKING` | 请求提交，等待首 token | 收到回答首 token 或错误 | 托腮、视线移动、思考 |
| `SPEAKING` | TTS/音频开始播放 | 音频播放结束/中断 | 说话、轻微节奏动作 |
| `WAITING_INPUT` | Agent 明确要求补充信息 | 用户继续输入/取消 | 举手、疑问表情 |
| `SUCCESS` | 一轮对话正常完成 | 定时短暂结束后 | 开心、庆祝 |
| `ERROR` | 网络/Provider/音频异常 | 用户重试/错误清除 | 困惑、轻度沮丧 |
| `SLEEP` | App 离开、息屏或低功耗 | 用户唤醒 | 闭眼、静息 |

优先级：`ERROR > LISTENING > SPEAKING > THINKING > WAITING_INPUT > SUCCESS > IDLE > SLEEP`。实际实现中 SLEEP 由 Power Manager 强制覆盖；ERROR 只在错误展示期间占优先级。SUCCESS 只短暂播放，随后回到 IDLE。

## 事件接口

动画层不直接访问网络或模型。业务层向 Companion Controller 发送事件：

```c
typedef enum {
    COMPANION_EVT_APP_ENTER,
    COMPANION_EVT_MIC_START,
    COMPANION_EVT_MIC_STOP,
    COMPANION_EVT_REQUEST_SUBMITTED,
    COMPANION_EVT_FIRST_TOKEN,
    COMPANION_EVT_TTS_START,
    COMPANION_EVT_TTS_END,
    COMPANION_EVT_NEED_USER_INPUT,
    COMPANION_EVT_TURN_DONE,
    COMPANION_EVT_ERROR,
    COMPANION_EVT_ERROR_CLEARED,
    COMPANION_EVT_APP_LEAVE,
    COMPANION_EVT_SCREEN_SLEEP,
    COMPANION_EVT_WAKE,
} companion_event_t;
```

`LISTENING` 只对应本地麦克风真实采集；`THINKING` 对应请求已提交但尚未收到回答；`SPEAKING` 对应 TTS/音频真实播放。纯文字回答时可显示 `THINKING`，收到完整文本后显示短暂 `SUCCESS`，不伪造口型。

## 两个角色的素材策略

### Muse

- 官方来源：<https://github.com/facebookincubator/muse-gadget-sdk>
- 官方 Avatar Recipe：<https://github.com/facebookincubator/muse-gadget-sdk/blob/main/esp32/tools/muse/AVATAR_RECIPE.md>
- 通过官方工具为用户自己的 Muse Avatar 生成 `muse_pixel.c`，并使用官方工具预览各状态 GIF。
- SDK 的默认 Jollybot 角色有独立 Meta copyright 声明，Apache-2.0 SDK License 不覆盖该角色。不要把默认 Jollybot 直接打包或重新发布。
- 用户提供的截图只保留为临时视觉参考，不作为最终产品素材。

### Bigfish 默认角色：大肥鱼

- 动作设计参考：<https://github.com/gmskywalker/deepseek-fat-fish-codex-pet>（idle / hover / working / success 等动作与 spritesheet 结构）。
- 角色来源与许可说明：<https://sbzyc.com/static/fish-pet/NOTICE.html>。鲸鱼娘原型 @上善无形；DeepSeek 女仆鲸鱼娘设计 @ZipZipPipe；相关形象/动画按 CC BY-NC-SA 4.0，禁止商用。
- 表情检索参考：<https://github.com/Loping151/dsh-nekowhale-stickers>；每张图片仍需保留其具体作者与授权信息。
- 未核验到具体动画文件的权利与授权前，只借鉴动作状态与时序，不将原图/动画放入固件或发行包。

## ESP32 资源规格

- 屏幕：480×480；角色显示区域初始建议高度 220–300 px，聊天文字和状态提示保留独立空间。
- 动画：普通动作目标 8–12 fps；待机动作 2–4 fps；短促反馈可 12–15 fps，需实测功耗。
- 原始素材：保留授权来源的透明 WebP/GIF/WebM，不在设备端直接解码 WebM；WebP 解码能力需单独验证。
- 固件资源：主机端转换成 RGB565 + Alpha Mask 或 ARGB1555 帧文件；同时生成 manifest（尺寸、帧数、帧时长、来源、作者、许可、SHA256）。
- 绘制：LVGL 局部刷新，尽量只更新角色矩形区域；避免整屏刷新。
- 内存：只保留当前角色当前动作所需帧；切换角色或息屏时释放/停止动画缓存。帧数据可放 PSRAM，DMA/显示缓冲按驱动要求放置。
- 低电量/息屏/后台：停止动画或降为单帧，不能为动画阻止 Power Manager 进入睡眠。

## 验收

1. 麦克风开始采集后 200 ms 内切换 LISTENING；停止采集后不残留聆听动画。
2. 请求提交后切换 THINKING；收到首 token 后退出 THINKING。
3. 仅在 TTS/音频真正开始播放时进入 SPEAKING；播放结束回到 SUCCESS/IDLE。
4. 网络错误进入 ERROR，重试后能恢复，不会卡死在错误动画。
5. 息屏后停止帧更新；唤醒后恢复 IDLE 或当前有效状态。
6. 连续切换 Muse / 大肥鱼 50 次无持续内存增长。
7. 480×480 屏幕下动画不遮挡主要聊天内容，帧率与功耗在设备实测后定版。


## 2026-11-14 新增概念素材

本次新增的 UI 概念图位于 `assets/ui/concepts/`；小智 Q 版角色九宫格位于 `assets/companions/xiaozhi/`；通用机器人动画测试图位于 `assets/companions/shared/`。这些素材用于讨论角色风格、状态动作和布局，不应视为已完成的 LVGL 运行时资源。

大肥鱼：本包保留大肥鱼素材目录，但未擅自从 dsh-pet 仓库复制受角色美术许可约束的动画文件。接入前请核对原仓库许可及署名要求。
