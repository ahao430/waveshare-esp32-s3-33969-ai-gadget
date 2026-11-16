/* ui_sound — UI 提示音统一入口（番茄钟按键/倒计时结束，2026-11-16）。
 * 双平台共享：真机=audio_esp32 正弦合成（ES8311）；模拟器=仅日志
 * （headless 回归钩子可断言"何时响了"）。
 * 两个接口都非阻塞（内部起短任务/只打日志），LVGL 线程直接调用安全。 */
#ifndef UI_SOUND_H
#define UI_SOUND_H

/* 按键反馈（短滴）：番茄钟 播放/重置/跳过 点击后调用。 */
void ui_sound_click(void);

/* 阶段提醒（三连滴）：番茄钟倒计时归零自动切阶段时调用；
 * 后台计时（页面已退出）同样生效。 */
void ui_sound_alert(void);

#endif /* UI_SOUND_H */
