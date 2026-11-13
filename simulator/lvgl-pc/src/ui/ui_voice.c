/* ui_voice — 语音助手演示页（挂 Muse App 位；PLAN §11 骨架演示）。
 * 光环动画 + PTT 按钮 + 状态文字，驱动 voice_session。
 * Xiaozhi 真机接入时复用本页模式（引擎名不同、共享同一 voice_session 租约）。
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "app_manager.h"
#include "audio_mock.h"
#include "data_source.h"
#include "voice_session.h"
#include "ui.h"
#include "ui_theme.h"

/* 录音缓存（Listening 期间 on_pcm 帧累积；上限 ~8s@16kHz） */
#define CAP_MAX_FRAMES (16000 * 8)
static int16_t g_capture[CAP_MAX_FRAMES];
static int g_capture_frames;

/* 页面控件（单前台模型：同刻只有一个实例） */
static lv_obj_t *g_ring;      /* 光环 arc */
static lv_obj_t *g_state_lbl;
static lv_obj_t *g_detail_lbl;
static lv_obj_t *g_speak_lbl;
static lv_obj_t *g_owner_lbl;
static lv_obj_t *g_ptt_btn;
static lv_obj_t *g_poll_timer;
static lv_obj_t *g_owner_lbl;
static char g_engine[24];

static bool g_inited; /* vs_init 全局一次 */

/* ---------------- voice_session listener（UI 线程） ---------------- */

static const char *state_text(vs_state_t st)
{
    switch (st) {
    case VS_LISTENING: return "聆听中…";
    case VS_THINKING:  return "思考中…";
    case VS_SPEAKING:  return "回答中";
    case VS_ERROR:     return "出错了";
    case VS_IDLE:      return "点按下方按钮说话";
    default:           return "";
    }
}

/* 后台 interact 线程状态（与 poll timer 的互斥由 worker→主线程单向写 +
 * poll 读） */
static pthread_mutex_t g_voice_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_voice_done;
static char g_voice_transcript[192];
static char g_voice_reply[512];

static void *voice_worker(void *arg)
{
    (void)arg;
    char transcript[192] = {0}, reply[512] = {0};
    ds_voice_result_t r = ds_voice_interact(g_capture, g_capture_frames,
                                            transcript, sizeof(transcript),
                                            reply, sizeof(reply));
    (void)r;
    pthread_mutex_lock(&g_voice_mu);
    strcpy(g_voice_transcript, transcript);
    strcpy(g_voice_reply, reply);
    g_voice_done = true;
    pthread_mutex_unlock(&g_voice_mu);
    return NULL;
}

static void on_vs_state(vs_state_t st, const char *detail, void *user)
{
    (void)user;
    if (!g_state_lbl) return; /* 页面已退出 */
    lv_label_set_text(g_state_lbl, state_text(st));
    lv_label_set_text(g_detail_lbl, detail);

    if (st == VS_LISTENING) {
        g_capture_frames = 0; /* 开始新录音 */
    }
    if (st == VS_THINKING) {
        /* Thinking：后台线程走 ASR→LLM→TTS 链路；完成经 poll timer 回 UI */
        pthread_t tid;
        pthread_create(&tid, NULL, voice_worker, NULL);
        pthread_detach(tid);
    }
}

static void on_vs_speak(const char *visible, void *user)
{
    (void)user;
    if (g_speak_lbl) {
        lv_label_set_text(g_speak_lbl, visible);
    }
}

static void on_vs_pcm(const int16_t *pcm, int frames, void *user)
{
    (void)user;
    /* 缓存采集帧（interact 用）；波形可视化 TODO */
    if (g_capture_frames + frames <= CAP_MAX_FRAMES) {
        memcpy(g_capture + g_capture_frames, pcm, (size_t)frames * 2);
        g_capture_frames += frames;
    }
}

static const vs_listener_t g_vs_listener = {
    .on_state = on_vs_state,
    .on_speak_progress = on_vs_speak,
    .on_pcm = on_vs_pcm,
    .user = NULL,
};

/* ---------------- PTT ---------------- */

static void ptt_pressed_cb(lv_event_t *e)
{
    (void)e;
    vs_ptt_begin();
}

static void ptt_released_cb(lv_event_t *e)
{
    (void)e;
    vs_ptt_end();
}

/* ---------------- tick 驱动 + 动画 ---------------- */

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    vs_tick();

    /* worker 完成检查（Thinking →Speaking 由 vs_notify_engine_done 触发） */
    pthread_mutex_lock(&g_voice_mu);
    bool done = g_voice_done;
    g_voice_done = false;
    char transcript[192], reply[512];
    if (done) {
        strcpy(transcript, g_voice_transcript);
        strcpy(reply, g_voice_reply);
    }
    pthread_mutex_unlock(&g_voice_mu);

    if (done && vs_state() == VS_THINKING) {
        /* 展示"听到的话"，并让 session 朗读 reply */
        char buf[256];
        snprintf(buf, sizeof(buf), "听到: %s", transcript);
        lv_label_set_text(g_speak_lbl, buf);
        vs_set_reply(reply);
        vs_notify_engine_done();
    }

    /* 光环随状态变化（无动画库调用，直接改值：低成本） */
    if (!g_ring) return;
    vs_state_t st = vs_state();
    lv_color_t ring_color = UI_CARD;
    int16_t ring_width = 14;
    switch (st) {
    case VS_LISTENING: ring_color = UI_OK;    ring_width = 20; break;
    case VS_THINKING:  ring_color = UI_WARN;  ring_width = 18; break;
    case VS_SPEAKING:  ring_color = UI_ACCENT; ring_width = 16; break;
    case VS_ERROR:     ring_color = UI_ERR;   ring_width = 22; break;
    case VS_IDLE:      ring_color = UI_CARD;  ring_width = 14; break;
    }
    lv_obj_set_style_arc_color(g_ring, ring_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_ring, ring_width, LV_PART_INDICATOR);
}

/* ---------------- 页面构建 ---------------- */

void ui_voice_build(lv_obj_t *scr, int app_id)
{
    /* vs_init 全局一次（音频 ops 装配点；固件侧换 audio_real_ops()） */
    if (!g_inited) {
        vs_init(audio_mock_ops());
        vs_set_listener(&g_vs_listener);
        g_inited = true;
    }

    /* 双引擎演示：Muse/小智共用本页，engine_id 不同 → 租约互斥
     * （ADR-003）。单前台模型下自然独占；真实引擎接入后此页换成
     * 各自的协议客户端 UI。 */
    const char *engine = (app_id == UI_APP_XIAOZHI) ? "xiaozhi-demo" : "muse-demo";
    const char *title = (app_id == UI_APP_XIAOZHI) ? "小智语音（演示）" : "Muse 语音（演示）";
    snprintf(g_engine, sizeof(g_engine), "%s", engine);

    if (!vs_acquire(g_engine)) {
        lv_obj_t *busy = ui_label(scr, "语音被其他引擎占用", &lv_font_cn_16, UI_ERR);
        lv_obj_center(busy);
        return;
    }

    lv_obj_t *title_lbl = ui_label(scr, title, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);

    /* 光环 + 吉祥物占位（吉祥物素材接入见 PLAN §8.5） */
    g_ring = lv_arc_create(scr);
    lv_obj_set_size(g_ring, 200, 200);
    lv_obj_align(g_ring, LV_ALIGN_CENTER, 0, -36);
    lv_arc_set_rotation(g_ring, 270);
    lv_arc_set_bg_angles(g_ring, 0, 360);
    lv_arc_set_range(g_ring, 0, 100);
    lv_arc_set_value(g_ring, 78);
    lv_obj_remove_style(g_ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_ring, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_ring, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_ring, UI_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_ring, UI_CARD, LV_PART_INDICATOR);

    lv_obj_t *mascot = ui_label(g_ring, LV_SYMBOL_AUDIO, &lv_font_montserrat_48, UI_ACCENT2);
    lv_obj_center(mascot);

    g_state_lbl = ui_label(scr, state_text(VS_IDLE), &lv_font_cn_16, UI_TEXT);
    lv_obj_align(g_state_lbl, LV_ALIGN_CENTER, 0, 62);

    g_speak_lbl = ui_label(scr, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(g_speak_lbl, LV_ALIGN_CENTER, 0, 90);
    lv_label_set_long_mode(g_speak_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_speak_lbl, 400);

    g_detail_lbl = ui_label(scr, "", &lv_font_montserrat_12, UI_TEXT_DIM);
    lv_obj_align(g_detail_lbl, LV_ALIGN_TOP_RIGHT, -8, 14);

    char ownerbuf[48];
    snprintf(ownerbuf, sizeof(ownerbuf), "engine: %s", g_engine);
    g_owner_lbl = ui_label(scr, ownerbuf, &lv_font_montserrat_12, UI_TEXT_DIM);
    lv_obj_align(g_owner_lbl, LV_ALIGN_TOP_RIGHT, -8, 32);

    /* PTT 大按钮（按下=开始聆听；松开=提交） */
    g_ptt_btn = lv_button_create(scr);
    lv_obj_set_size(g_ptt_btn, LV_PCT(100) - 16, 56);
    lv_obj_align(g_ptt_btn, LV_ALIGN_BOTTOM_MID, 0, 8);
    ui_style_card(g_ptt_btn);
    lv_obj_set_style_bg_color(g_ptt_btn, UI_ACCENT, 0);
    lv_obj_set_style_radius(g_ptt_btn, 18, 0);
    lv_obj_add_event_cb(g_ptt_btn, ptt_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_ptt_btn, ptt_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *ptt_lbl = ui_label(g_ptt_btn, "按住说话", &lv_font_cn_16, UI_TEXT);
    lv_obj_center(ptt_lbl);

    /* tick 驱动 + 光环状态刷新（页面销毁时 on_exit 停止） */
    g_poll_timer = lv_timer_create(poll_timer_cb, 50, NULL);
}

void ui_voice_on_exit(void)
{
    if (g_poll_timer) {
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }
    vs_release();
    g_ring = NULL;
    g_state_lbl = NULL;
    g_speak_lbl = NULL;
    g_detail_lbl = NULL;
    g_ptt_btn = NULL;
}
