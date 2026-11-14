/* ui_conversation — 对话类 App 统一壳（用户反馈定稿形态）：
 *   [back] [标题]            ← 无文字输入框
 *   对话历史（气泡，滚动）
 *   动态形象（吉祥物 GIF）
 *   状态行 + 波形条
 *   [按住说话]               ← 点击/按住进入语音对话
 *
 * ZCode/大肥鱼/Muse/小智 四 App 共用本壳：
 *   - 引擎 id/标题不同（voice_session 租约互斥，ADR-003）
 *   - 大肥鱼/小智/Muse：按住说话走 ds_voice_interact（ASR→LLM→TTS）
 *   - ZCode：数据源待定（PLAN），语音按钮禁用、历史为占位文案
 * 历史按 App 独立保存（静态缓冲），退出重进保留。
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "audio_mock.h"
#include "data_source.h"
#include "voice_session.h"
#include "ui.h"
#include "ui_theme.h"

#define CONV_MSGS 12
#define CONV_TEXT 384
#define WAVE_BARS_MAX 32

typedef struct {
    char role[12];
    char text[CONV_TEXT];
} conv_msg_t;

/* 每 App 独立历史 */
static conv_msg_t g_hist[UI_APP_COUNT][CONV_MSGS];
static int g_hist_n[UI_APP_COUNT];

/* 页面控件（单前台） */
static lv_obj_t *g_hist_area;
static lv_obj_t *g_status_lbl;
static lv_obj_t *g_reply_lbl;
static lv_obj_t *g_wave[32];
static lv_obj_t *g_ptt_btn;
static lv_obj_t *g_poll_timer;
static lv_obj_t *g_mascot;
static int g_app_id = -1;
static bool g_voice_enabled;

/* voice_session 状态（从 ui_voice 移植） */
static bool g_vs_inited;
static pthread_mutex_t g_voice_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_voice_done;
static char g_transcript[192];
static char g_reply[CONV_TEXT];
static int16_t g_capture[16000 * 8];
static int g_capture_frames;
static int g_wave_amp;
static bool g_wave_data;

#define WAVE_BARS_MAX 32

static const vs_listener_t g_listener;

static const struct {
    const char *engine;
    const char *title;
    bool voice;
} g_meta[UI_APP_COUNT] = {
    [UI_APP_ZCODE]   = { "zcode",      "ZCode 会话", false },
    [UI_APP_CHAT]    = { "bigfish",    "大肥鱼",     true  },
    [UI_APP_MUSE]    = { "muse",       "Muse",       true  },
    [UI_APP_XIAOZHI] = { "xiaozhi",    "小智",       true  },
};

/* ---------------- voice_session 监听 ---------------- */

static const char *state_text(vs_state_t st)
{
    switch (st) {
    case VS_LISTENING: return "聆听中…";
    case VS_THINKING:  return "思考中…";
    case VS_SPEAKING:  return "回答中";
    case VS_ERROR:     return "出错了";
    case VS_IDLE:      return "按住下方按钮说话";
    default:           return "";
    }
}

static void *voice_worker(void *arg)
{
    (void)arg;
    char transcript[192] = {0}, reply[CONV_TEXT] = {0};
    ds_voice_interact(g_capture, g_capture_frames,
                      transcript, sizeof(transcript),
                      reply, sizeof(reply));

    /* 写入历史（transcript 作为用户气泡 + reply 作为助手气泡） */
    int id = g_app_id;
    if (id >= 0 && id < UI_APP_COUNT) {
        conv_msg_t *h = g_hist[id];
        int *n = &g_hist_n[id];
        if (*n >= CONV_MSGS - 1) {
            memmove(h, h + 2, sizeof(conv_msg_t) * (*n - 2));
            *n -= 2;
        }
        snprintf(h[*n].role, sizeof(h[*n].role), "user");
        snprintf(h[*n].text, sizeof(h[*n].text), "%.127s", transcript);
        (*n)++;
        strcpy(h[*n].role, "assistant");
        snprintf(h[*n].text, sizeof(h[*n].text), "%.*s", CONV_TEXT - 1, reply);
        (*n)++;
    }

    pthread_mutex_lock(&g_voice_mu);
    strcpy(g_transcript, transcript);
    strcpy(g_reply, reply);
    g_voice_done = true;
    pthread_mutex_unlock(&g_voice_mu);
    return NULL;
}

static void on_vs_state(vs_state_t st, const char *detail, void *user)
{
    (void)user;
    if (!g_status_lbl) return;
    lv_label_set_text(g_status_lbl, state_text(st));

    if (st == VS_LISTENING) g_capture_frames = 0;
    if (st == VS_THINKING) {
        pthread_t tid;
        pthread_create(&tid, NULL, voice_worker, NULL);
        pthread_detach(tid);
    }
}

static void on_vs_speak(const char *visible, void *user)
{
    (void)user;
    if (g_reply_lbl) lv_label_set_text(g_reply_lbl, visible);
}

static void on_vs_pcm(const int16_t *pcm, int frames, void *user)
{
    (void)user;
    if (g_capture_frames + frames <= (int)(sizeof(g_capture) / sizeof(g_capture[0]))) {
        memcpy(g_capture + g_capture_frames, pcm, (size_t)frames * 2);
        g_capture_frames += frames;
    }
    double sum = 0;
    for (int i = 0; i < frames; i++) sum += (double)pcm[i] * pcm[i];
    double rms = sqrt(sum / frames) / 32768.0;
    g_wave_amp = (int)(rms * 300);
    if (g_wave_amp > 100) g_wave_amp = 100;
    g_wave_data = true;
}

static const vs_listener_t g_listener = {
    .on_state = on_vs_state,
    .on_speak_progress = on_vs_speak,
    .on_pcm = on_vs_pcm,
    .user = NULL,
};

/* ---------------- 气泡 ---------------- */

static void add_bubble(lv_obj_t *parent, const char *role, const char *text)
{
    bool is_user = strcmp(role, "user") == 0;
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_END, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_style_bg_color(bubble, is_user ? UI_ACCENT : UI_CARD, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = ui_label(bubble, text, &lv_font_cn_14, UI_TEXT);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(bubble, 300, 0);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
}

static void rerender_history(void)
{
    if (!g_hist_area) return;
    lv_obj_clean(g_hist_area);
    for (int i = 0; i < g_hist_n[g_app_id]; i++) {
        add_bubble(g_hist_area, g_hist[g_app_id][i].role, g_hist[g_app_id][i].text);
    }
    lv_obj_scroll_to_y(g_hist_area, LV_COORD_MAX, LV_ANIM_OFF);
}

/* ---------------- PTT / poll ---------------- */

static void ptt_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (g_voice_enabled) vs_ptt_begin();
}

static void ptt_released_cb(lv_event_t *e)
{
    (void)e;
    if (g_voice_enabled) vs_ptt_end();
}

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    vs_tick();

    pthread_mutex_lock(&g_voice_mu);
    bool done = g_voice_done;
    g_voice_done = false;
    char reply[CONV_TEXT];
    if (done) strcpy(reply, g_reply);
    pthread_mutex_unlock(&g_voice_mu);

    if (done && vs_state() == VS_THINKING) {
        if (g_reply_lbl) {
            char buf[CONV_TEXT + 16];
            snprintf(buf, sizeof(buf), "%s", g_transcript);
            lv_label_set_text(g_reply_lbl, buf);
        }
        vs_set_reply(reply);
        vs_notify_engine_done();
        rerender_history(); /* transcript/reply 已入历史 */
    }

    /* 波形 */
    static int disp;
    if (g_wave_data) {
        int target = (vs_state() == VS_LISTENING) ? g_wave_amp : 0;
        disp += (target - disp) / 2;
        for (int i = 0; i < WAVE_BARS_MAX; i++) {
            if (!g_wave[i]) continue;
            int h = 4 + disp * 3 / 4;
            if (h > 12) h += (int)(6 * (0.5 + 0.5 * sin(i * 0.7 + (double)disp / 6)));
            if (h < 4) h = 4;
            lv_obj_set_size(g_wave[i], 6, h);
            lv_obj_align(g_wave[i], LV_ALIGN_BOTTOM_MID,
                         -((WAVE_BARS_MAX / 2) * 8) + i * 8, 116);
            lv_obj_set_style_bg_opa(g_wave[i],
                                    vs_state() == VS_LISTENING ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

/* ---------------- 页面构建 ---------------- */

void ui_conversation_build(lv_obj_t *scr, int app_id)
{
    g_app_id = app_id;
    g_voice_enabled = g_meta[app_id].voice;

    if (!g_vs_inited) {
        vs_init(audio_mock_ops());
        vs_set_listener(&g_listener);
        vs_set_mock_capture(true);
        g_vs_inited = true;
    }
    if (!vs_acquire(g_meta[app_id].engine)) {
        lv_obj_t *busy = ui_label(scr, "语音被其他引擎占用", &lv_font_cn_16, UI_ERR);
        lv_obj_center(busy);
        return;
    }

    lv_obj_t *title_lbl = ui_label(scr, g_meta[app_id].title, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);

    /* 历史区（滚动，占上部 180px） */
    g_hist_area = lv_obj_create(scr);
    lv_obj_set_size(g_hist_area, LV_PCT(100), 180);
    lv_obj_align(g_hist_area, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_opa(g_hist_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_hist_area, 0, 0);
    lv_obj_set_style_pad_all(g_hist_area, 2, 0);
    lv_obj_set_flex_flow(g_hist_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_hist_area, 6, 0);

    /* 形象（吉祥物 GIF，96×96）+ 状态 */
    g_mascot = lv_gif_create(scr);
    lv_gif_set_src(g_mascot, "A:simulator/lvgl-pc/assets/mascot.gif");
    /* gif 原始 439×391：scale 用 256 基准 zoom（110/439 ≈ 64） */
    lv_obj_set_size(g_mascot, 110, 110);
    lv_image_set_scale(g_mascot, 64);
    lv_obj_align(g_mascot, LV_ALIGN_TOP_MID, 0, 240);

    g_status_lbl = ui_label(scr, state_text(VS_IDLE), &lv_font_cn_14, UI_TEXT);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_MID, 0, 344);

    g_reply_lbl = ui_label(scr, "", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(g_reply_lbl, LV_ALIGN_TOP_MID, 0, 366);
    lv_label_set_long_mode(g_reply_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_reply_lbl, 440);
    lv_label_set_long_mode(g_reply_lbl, LV_LABEL_LONG_DOT);

    /* 波形（状态行左右两侧，各 16 条） */
    for (int i = 0; i < WAVE_BARS_MAX; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_set_size(bar, 6, 4);
        lv_obj_set_style_bg_color(bar, UI_ACCENT2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, -((WAVE_BARS_MAX / 2) * 8) + i * 8, 116);
        g_wave[i] = bar;
    }

    /* PTT */
    g_ptt_btn = lv_button_create(scr);
    lv_obj_set_size(g_ptt_btn, LV_PCT(100) - 16, 52);
    lv_obj_align(g_ptt_btn, LV_ALIGN_BOTTOM_MID, 0, 8);
    ui_style_card(g_ptt_btn);
    lv_obj_set_style_bg_color(g_ptt_btn, g_voice_enabled ? UI_ACCENT : UI_CARD, 0);
    lv_obj_set_style_radius(g_ptt_btn, 16, 0);
    if (g_voice_enabled) {
        lv_obj_add_event_cb(g_ptt_btn, ptt_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(g_ptt_btn, ptt_released_cb, LV_EVENT_RELEASED, NULL);
    }
    lv_obj_t *ptt_lbl = ui_label(g_ptt_btn,
                                 g_voice_enabled ? "按住说话" : "语音对话 · 等待数据源",
                                 &lv_font_cn_16, g_voice_enabled ? UI_TEXT : UI_TEXT_DIM);
    lv_obj_center(ptt_lbl);

    if (!g_voice_enabled) {
        lv_obj_t *hint = ui_label(scr, "ZCode 数据源待接入（PLAN §5.7）",
                                  &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 344);
        lv_obj_set_style_text_color(hint, UI_TEXT_DIM, 0);
        lv_obj_del(g_status_lbl);
        g_status_lbl = NULL;
    }

    rerender_history();

    g_poll_timer = lv_timer_create(poll_timer_cb, 50, NULL);
}

void ui_conversation_on_exit(void)
{
    if (g_poll_timer) {
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }
    vs_release();
    g_hist_area = NULL;
    g_status_lbl = NULL;
    g_reply_lbl = NULL;
    g_ptt_btn = NULL;
    g_mascot = NULL;
    for (int i = 0; i < WAVE_BARS_MAX; i++) g_wave[i] = NULL;
    g_wave_data = false;
    g_app_id = -1;
}
