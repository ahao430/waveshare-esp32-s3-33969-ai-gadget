/* companion — 公共角色动画引擎（ADR-005：Shared Assistant UI 的形象层）。
 * 统一状态接口（与 voice_session 状态机同源）→ 每 App 独立资源映射。
 *
 * 资源优先级（COMPANION_ASSETS.md §2 规范目录）：
 *   1. PNG 序列帧 companions/<char>/<action>/frame_NN.png（原生 alpha，
 *      gifdec 调色板透明两次渲染成绿色后全面切换；scripts/
 *      companion_gif2frames.sh 从过渡期 GIF 生成）
 *   2. 旧 GIF 路径回退（帧目录缺失时）
 * 服务适配器只发状态事件（voice_session listener），换形象/加动作
 * 不动聊天引擎。固件侧对应 components/companion_engine/。
 */
#include "companion.h"

#include <stdio.h>
#include <string.h>

#include "ui.h" /* UI_APP_* 枚举 */

#define COMPANION_ROOT "A:simulator/lvgl-pc/assets/companions"

/* 每帧集合（dir 相对 COMPANION_ROOT；frames 运行时探测） */
typedef struct {
    const char *dir;
    int frames;
} frame_set_t;

typedef struct {
    int app_id;
    const frame_set_t *set;   /* 当前播放集合 */
    int idx;
} frames_ctx_t;

static lv_obj_t *g_obj;      /* 帧画面（lv_image） */
static lv_timer_t *g_timer;
static frames_ctx_t g_ctx;
static int g_app = -1;

/* 大肥鱼：voice_session 三态 → 三个动作集（过渡期映射，
 * 8 动作全量映射见 COMPANION_ASSETS §4 计划） */
static frame_set_t k_set_bigfish_idle = { "bigfish/idle", 0 };
static frame_set_t k_set_bigfish_think = { "bigfish/thinking", 0 };
static frame_set_t k_set_bigfish_speak = { "bigfish/speaking", 0 };
static frame_set_t k_set_xiaozhi_wave = { "xiaozhi/wave", 0 };
static frame_set_t k_set_muse_idle = { "muse/idle", 0 };

/* 探测目录帧数（frame_00.png 起顺序存在） */
static int probe_frames(const char *dir_rel)
{
    char path[160];
    int n = 0;
    while (n < 99) {
        snprintf(path, sizeof(path), "simulator/lvgl-pc/assets/companions/%s/frame_%02d.png",
                 dir_rel, n);
        FILE *f = fopen(path, "rb");
        if (!f) break;
        fclose(f);
        n++;
    }
    return n;
}

static const frame_set_t *set_for(int app_id, vs_state_t st)
{
    static bool probed = false;
    if (!probed) { /* 一次性探测（目录在运行期不变） */
        k_set_bigfish_idle.frames = probe_frames(k_set_bigfish_idle.dir);
        k_set_bigfish_think.frames = probe_frames(k_set_bigfish_think.dir);
        k_set_bigfish_speak.frames = probe_frames(k_set_bigfish_speak.dir);
        k_set_xiaozhi_wave.frames = probe_frames(k_set_xiaozhi_wave.dir);
        k_set_muse_idle.frames = probe_frames(k_set_muse_idle.dir);
        probed = true;
    }

    switch (app_id) {
    case UI_APP_CHAT:
        if (st == VS_THINKING && k_set_bigfish_think.frames > 0) return &k_set_bigfish_think;
        if (st == VS_SPEAKING && k_set_bigfish_speak.frames > 0) return &k_set_bigfish_speak;
        return k_set_bigfish_idle.frames > 0 ? &k_set_bigfish_idle : NULL;
    case UI_APP_XIAOZHI:
        return k_set_xiaozhi_wave.frames > 0 ? &k_set_xiaozhi_wave : NULL;
    case UI_APP_MUSE:
        return k_set_muse_idle.frames > 0 ? &k_set_muse_idle : NULL;
    default:
        return NULL;
    }
}

/* 每角色显示几何（沿用过渡期 GIF 观感） */
static void apply_geometry(int app_id, lv_obj_t *img)
{
    switch (app_id) {
    case UI_APP_CHAT: /* 源帧 144×82 → 200×114 */
        lv_obj_set_size(img, 200, 114);
        lv_image_set_scale(img, (256 * 200) / 144);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 96);
        break;
    case UI_APP_XIAOZHI: /* 220×195 原尺寸 */
        lv_obj_set_size(img, 220, 195);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 92);
        break;
    case UI_APP_MUSE: /* 439×391 → 150 */
        lv_obj_set_size(img, 150, 150);
        lv_image_set_scale(img, (256 * 150) / 439);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 80);
        break;
    }
}

static void frames_show(const frame_set_t *set, int idx)
{
    char src[160];
    snprintf(src, sizeof(src), "%s/%s/frame_%02d.png", COMPANION_ROOT, set->dir, idx);
    lv_image_set_src(g_obj, src);
}

static void frames_timer_cb(lv_timer_t *t)
{
    frames_ctx_t *ctx = lv_timer_get_user_data(t);
    ctx->idx = (ctx->idx + 1) % ctx->set->frames;
    frames_show(ctx->set, ctx->idx);
}

/* ---- 旧 GIF 回退（帧目录缺失时；gifdec 调色板问题见文件头） ---- */

static const char *dsh_path(vs_state_t st)
{
    switch (st) {
    case VS_THINKING: return "A:simulator/lvgl-pc/assets/dsh/dsh-typing.gif";
    case VS_SPEAKING: return "A:simulator/lvgl-pc/assets/dsh/dsh-sparkle.gif";
    default:          return "A:simulator/lvgl-pc/assets/dsh/dsh-idle.gif";
    }
}

static lv_obj_t *gif_fallback(lv_obj_t *parent, int app_id, vs_state_t st)
{
    lv_obj_t *gif = lv_gif_create(parent);
    switch (app_id) {
    case UI_APP_CHAT:
        lv_obj_set_size(gif, 200, 114);
        lv_image_set_scale(gif, (256 * 200) / 144);
        lv_gif_set_src(gif, dsh_path(st));
        lv_obj_align(gif, LV_ALIGN_TOP_MID, 0, 96);
        break;
    case UI_APP_XIAOZHI:
        lv_obj_set_size(gif, 220, 195);
        lv_gif_set_src(gif, "A:simulator/lvgl-pc/assets/xiaozhi/xiaozhi-wave.gif");
        lv_obj_align(gif, LV_ALIGN_TOP_MID, 0, 92);
        break;
    case UI_APP_MUSE:
        lv_obj_set_size(gif, 150, 150);
        lv_image_set_scale(gif, (256 * 150) / 439);
        lv_gif_set_src(gif, "A:simulator/lvgl-pc/assets/mascot.gif");
        lv_obj_align(gif, LV_ALIGN_TOP_MID, 0, 80);
        break;
    }
    return gif;
}

void companion_create(lv_obj_t *parent, int app_id, int16_t y)
{
    (void)y; /* 几何按 apply_geometry（帧方案后无需外部 y） */
    companion_delete();

    g_app = app_id;
    const frame_set_t *set = set_for(app_id, VS_IDLE);
    if (!set) {
        g_obj = gif_fallback(parent, app_id, VS_IDLE); /* 帧缺失 → GIF 回退 */
        return;
    }

    g_obj = lv_image_create(parent);
    apply_geometry(app_id, g_obj);
    g_ctx.app_id = app_id;
    g_ctx.set = set;
    g_ctx.idx = 0;
    frames_show(set, 0);
    g_timer = lv_timer_create(frames_timer_cb, 125, &g_ctx); /* 8fps */
}

void companion_set_state(int app_id, vs_state_t st)
{
    if (app_id != g_app || !g_obj) return;

    const frame_set_t *set = set_for(app_id, st);
    if (set && g_timer) { /* 帧模式：切换动作集 */
        if (set != g_ctx.set) {
            g_ctx.set = set;
            g_ctx.idx = 0;
            frames_show(set, 0);
        }
        return;
    }

    /* GIF 回退模式：仅大肥鱼有多态资源 */
    if (g_timer == NULL && app_id == UI_APP_CHAT) {
        lv_gif_set_src(g_obj, dsh_path(st));
    }
}

void companion_delete(void)
{
    if (g_timer) {
        lv_timer_delete(g_timer);
        g_timer = NULL;
    }
    if (g_obj) {
        lv_obj_delete(g_obj);
        g_obj = NULL;
    }
    g_app = -1;
}

/* ---- 8 类动作接口（docs/COMPANION_ASSETS.md §1）----
 * 逐帧美术资源产出前的映射：voice 5 态直映；happy→speaking；
 * surprised/wave/sleep→idle（once 语义待资源就绪后实现）。 */
void companion_set_action(int app_id, companion_action_t act)
{
    switch (act) {
    case COMPANION_ACT_LISTENING:
    case COMPANION_ACT_THINKING:
    case COMPANION_ACT_SPEAKING:
    case COMPANION_ACT_IDLE:
        companion_set_state(app_id, (vs_state_t)act);
        break;
    case COMPANION_ACT_HAPPY:
        companion_set_state(app_id, VS_SPEAKING);
        break;
    case COMPANION_ACT_SURPRISED:
    case COMPANION_ACT_WAVE:
    case COMPANION_ACT_SLEEP:
    default:
        companion_set_state(app_id, VS_IDLE);
        break;
    }
}
