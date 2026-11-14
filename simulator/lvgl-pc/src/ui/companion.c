/* companion — 见 companion.h（ADR-005 角色动画引擎）。 */
#include "companion.h"

#include <string.h>

#include "ui.h" /* UI_APP_* 枚举 */

/* 每 App 形象形态 */
#define KIND_NONE  0 /* 不显示形象 */
#define KIND_DSH   1 /* dsh 状态动画（黑底屏幕卡） */
#define KIND_GIF   2 /* 单 GIF（Muse/robot） */
#define KIND_IMAGE 3 /* 静态 PNG（小智） */

static const uint8_t g_kinds[UI_APP_COUNT] = {
    [UI_APP_CHAT]    = KIND_DSH,
    [UI_APP_MUSE]    = KIND_GIF,
    [UI_APP_XIAOZHI] = KIND_IMAGE,
    /* 其余（launcher/zcode/monitor/...）：无形象 */
};

static lv_obj_t *g_obj;
static lv_obj_t *g_stage;
static int g_app = -1;

/* dsh 资源按状态 */
static const char *dsh_path(vs_state_t st)
{
    switch (st) {
    case VS_THINKING: return "A:simulator/lvgl-pc/assets/dsh/dsh-typing.gif";
    case VS_SPEAKING: return "A:simulator/lvgl-pc/assets/dsh/dsh-sparkle.gif";
    default:          return "A:simulator/lvgl-pc/assets/dsh/dsh-idle.gif";
    }
}

static void create_dsh(lv_obj_t *parent, int16_t y)
{
    g_stage = lv_obj_create(parent);
    lv_obj_set_size(g_stage, 216, 130);
    lv_obj_set_style_bg_color(g_stage, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_stage, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_stage, 16, 0);
    lv_obj_set_style_border_width(g_stage, 0, 0);
    lv_obj_set_style_shadow_width(g_stage, 0, 0);
    lv_obj_clear_flag(g_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g_stage, LV_ALIGN_TOP_MID, 0, y);

    g_obj = lv_gif_create(parent);
    lv_obj_set_size(g_obj, 200, 114);
    lv_image_set_scale(g_obj, (256 * 200) / 144);
    lv_gif_set_src(g_obj, dsh_path(VS_IDLE));
    lv_obj_align(g_obj, LV_ALIGN_TOP_MID, 0, y + 8);
}

static void create_gif(lv_obj_t *parent, int16_t y)
{
    g_obj = lv_gif_create(parent);
    lv_obj_set_size(g_obj, 150, 150);
    lv_image_set_scale(g_obj, (256 * 150) / 439); /* robot 439×391 → 150 */
    lv_gif_set_src(g_obj, "A:simulator/lvgl-pc/assets/mascot.gif");
    lv_obj_align(g_obj, LV_ALIGN_TOP_MID, 0, y);
}

/* 小智形象：素材包 wave 动画（"png" 实为 GIF 89a 动画） */
static void create_image(lv_obj_t *parent, int16_t y)
{
    g_obj = lv_gif_create(parent);
    lv_gif_set_src(g_obj, "A:simulator/lvgl-pc/assets/xiaozhi/xiaozhi-wave.gif");
    lv_obj_set_size(g_obj, 220, 195);
    lv_obj_align(g_obj, LV_ALIGN_TOP_MID, 0, y + 12);
}

void companion_create(lv_obj_t *parent, int app_id, int16_t y)
{
    companion_delete();

    g_app = app_id;
    uint8_t kind = (app_id >= 0 && app_id < UI_APP_COUNT)
                       ? g_kinds[app_id]
                       : KIND_NONE;

    switch (kind) {
    case KIND_DSH:
        create_dsh(parent, y);
        break;
    case KIND_GIF:
        create_gif(parent, y);
        break;
    case KIND_IMAGE:
        create_image(parent, y);
        break;
    case KIND_NONE:
    default:
        g_obj = NULL;
        break;
    }
}

void companion_set_state(int app_id, vs_state_t st)
{
    if (app_id != g_app || !g_obj) return;
    if (g_kinds[app_id] == KIND_DSH) {
        lv_gif_set_src(g_obj, dsh_path(st));
    }
    /* GIF/IMAGE 单资源形态：状态由波形/字幕表达，不切资源 */
}

void companion_delete(void)
{
    if (g_stage) {
        lv_obj_delete(g_stage);
        g_stage = NULL;
    }
    if (g_obj) {
        lv_obj_delete(g_obj);
        g_obj = NULL;
    }
    g_app = -1;
}

/* ---- 8 类动作接口（docs/COMPANION_ASSETS.md §1）----
 * 逐帧资源管线（frame_NN.bin）尚未产出：本节先定义状态→动作映射与
 * once 播放语义；资源就绪后在此表接入 manifest 驱动的逐帧播放器。 */
void companion_set_action(int app_id, companion_action_t act)
{
    /* voice_session 5 态直映；扩展态当前由各资源形态降级处理：
     * happy → speaking 动画（播放一次的语义由资源层实现），
     * surprised/wave/sleep → idle（资源未就绪）。 */
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
