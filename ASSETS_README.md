# 设计素材说明

- `assets/ui/device-ui-concept.png`：设备端桌面、订阅额度、服务器监控概念图。
- `assets/mascots/muse-companion-concept.gif`：原创 AI 伙伴动画草案，不是 Muse 官方 Logo。

供应商商标请从各品牌官方资源页获取；不要将生成图当成供应商官方 Logo。

## 大肥鱼状态动画（2026-11-14 新增）

- 来源：[PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)（MIT）
- 素材：`dsh-pet/assets/webm/` 状态动画 → ffmpeg 转 144×82 / 10fps GIF
  （黑底；webm 的透明在 Electron 播放层实现，文件本身 yuv420p）
- 状态映射：Idle/Listening=东张西望、Thinking=写代码、Speaking=凭空生花
- 位置：`simulator/lvgl-pc/assets/dsh/`，UI 加黑色圆角卡展示；
  会话状态切换动画见 `ui_conversation.c set_mascot_for_state()`
