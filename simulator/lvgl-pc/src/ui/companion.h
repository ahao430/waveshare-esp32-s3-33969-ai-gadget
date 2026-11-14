/* companion — 公共角色动画引擎（ADR-005：Shared Assistant UI 的形象层）。
 * 统一状态接口（与 voice_session 状态机同源）→ 每 App 独立资源映射：
 *   大肥鱼  = dsh-pet 状态动画（idle/typing/sparkle，黑底屏幕卡）
 *   小智    = Q 版台湾女生静态形象（gpt-image 生成）
 *   Muse 等 = 吉祥物 GIF
 * 服务适配器只发状态事件（voice_session listener），换形象/加动作
 * 不动聊天引擎。固件侧对应 components/companion_engine/。
 */
#ifndef COMPANION_H
#define COMPANION_H

#include <stdint.h>

#include "lvgl.h"
#include "voice_session.h"

/* 8 类动作（docs/COMPANION_ASSETS.md §1）：三角色共用接口、独立资源。
 * 前 5 个与 voice_session 状态一一对应；扩展态由系统事件触发。 */
typedef enum {
    COMPANION_ACT_IDLE = 0,
    COMPANION_ACT_LISTENING,
    COMPANION_ACT_THINKING,
    COMPANION_ACT_SPEAKING,
    COMPANION_ACT_HAPPY,     /* 播放一次：回复完成 */
    COMPANION_ACT_SURPRISED, /* 播放一次：事件提醒 */
    COMPANION_ACT_WAVE,      /* 播放一次：启动/打招呼 */
    COMPANION_ACT_SLEEP,     /* 循环：屏幕休眠 */
    COMPANION_ACT_COUNT,
} companion_action_t;

typedef enum {
    COMPANION_PLAY_LOOP = 0,
    COMPANION_PLAY_ONCE, /* 播完回 idle */
} companion_play_t;

/* 在 parent 上创建形象（含 dsh 屏幕卡），y 为 TOP_MID 偏移。
 * app_id 不在支持表内时不创建（companion_get() 返回 NULL）。 */
void companion_create(lv_obj_t *parent, int app_id, int16_t y);

/* 状态变化 → 切换资源（仅大肥鱼有多态资源；其余形象静态）。
 * 兼容接口：voice_session 5 态直传；8 态用 companion_set_action。 */
void companion_set_state(int app_id, vs_state_t st);

/* 8 类动作接口（资源管线就绪后启用逐帧播放） */
void companion_set_action(int app_id, companion_action_t act);

/* 删除形象与屏幕卡（退出 App 时） */
void companion_delete(void);

#endif /* COMPANION_H */
