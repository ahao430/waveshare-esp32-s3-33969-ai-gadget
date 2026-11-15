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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"

#ifdef ESP_PLATFORM
#include "audio_esp32.h"
#else
#include "audio_mock.h"
#endif

#include "companion.h"
#include "data_source.h"
#include "voice_session.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_keys.h"
#include "ui_statusbar.h"
#include "ui_theme.h"

/* PTT 事件留痕：按下/抬起/丢失任何一个没到，表现都是"按住说话没反应"
 *（无 HTTP 请求、无报错），日志是唯一判据。 */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define CLOG(fmt, ...) ESP_LOGI("conv", fmt, ##__VA_ARGS__)
#else
#define CLOG(fmt, ...) fprintf(stderr, "[conv] " fmt "\n", ##__VA_ARGS__)
#endif

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

/* AstrBot 多实例：聊天记录再按机器人实例分槽（用户 2026-11-16 反馈
 * "不同机器人对话历史共享"）。每 App 4.6KB×12 条放静态数组会把内部
 * RAM 吃穿，8 实例 ×12 条 ≈37KB 放 PSRAM；分配失败回落共享槽。
 * 服务器侧会话本就按实例隔离（Bridge username/session 每实例独立）。 */
#define BOT_HIST_BOTS 8
static conv_msg_t *s_bot_hist;               /* 惰性分配（PSRAM/PC 堆） */
static int s_bot_hist_n[BOT_HIST_BOTS];
static char s_bot_hist_id[BOT_HIST_BOTS][64];
static char g_active_bot_id[64];             /* 当前机器人实例 id（sync/切换时更新） */

static void bot_hist_ensure(void)
{
    if (s_bot_hist) return;
    size_t bytes = (size_t)BOT_HIST_BOTS * CONV_MSGS * sizeof(conv_msg_t);
#ifdef ESP_PLATFORM
    s_bot_hist = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
    s_bot_hist = malloc(bytes);
#endif
    if (s_bot_hist) memset(s_bot_hist, 0, bytes);
}

static int bot_hist_slot(const char *bot_id)
{
    if (!bot_id || !bot_id[0]) return -1;
    for (int i = 0; i < BOT_HIST_BOTS; i++) {
        if (s_bot_hist_id[i][0] == '\0') {
            snprintf(s_bot_hist_id[i], sizeof(s_bot_hist_id[i]), "%s", bot_id);
            s_bot_hist_n[i] = 0;
            return i;
        }
        if (strcmp(s_bot_hist_id[i], bot_id) == 0) return i;
    }
    return -1; /* 槽满：回落共享历史 */
}

/* 取当前 App 的历史槽：chat App 优先按机器人实例；其余按 App。
 * h/n 永不为 NULL（回落 g_hist）。 */
static void conv_hist_slot(int app_id, conv_msg_t **h, int **n)
{
    *h = NULL;
    *n = NULL;
    if (app_id < 0 || app_id >= UI_APP_COUNT) return;
    if (app_id == UI_APP_CHAT) {
        bot_hist_ensure();
        int slot = bot_hist_slot(g_active_bot_id);
        if (slot >= 0 && s_bot_hist) {
            *h = &s_bot_hist[(size_t)slot * CONV_MSGS];
            *n = &s_bot_hist_n[slot];
            return;
        }
    }
    *h = g_hist[app_id];
    *n = &g_hist_n[app_id];
}

/* 页面控件（单前台） */
static lv_obj_t *g_status_lbl;
static lv_obj_t *g_reply_lbl;
static lv_obj_t *g_title_lbl;   /* AstrBot 页：标题兼"切换机器人"入口 */
static char g_bot_label[32];    /* 当前机器人名称（切换后显示在标题） */
static lv_obj_t *g_wave[32];
static lv_obj_t *g_ptt_btn;
static lv_obj_t *g_cont_lbl;
static lv_timer_t *g_poll_timer;
static int g_app_id = -1;
static bool g_voice_enabled;
static bool g_continuous; /* 连续对话（KEY3 短按/左上角按钮切换） */
static bool g_cont_auto;      /* 当前 LISTENING 轮由连续模式自动开启 */
static bool g_cont_heard;     /* 本轮已检测到人声（VAD） */
static int g_cont_maxamp;     /* 本轮最大幅度（诊断/调阈值） */
static uint32_t g_cont_loud_tick; /* 最后一次有声时刻（VAD 静音判据） */

/* 连续对话参数（80ms poll 驱动；g_wave_amp 0..100）。
 * 首版绝对阈值 10 真机全轮未触发（麦克风电平低）——改自适应：
 * 有声 = amp ≥ 底噪+5 且 ≥4；底噪 EMA 只在判无声时更新。 */
#define CONT_RESPEAK_GAP_MS 800  /* 回复播完 → 自动开听的间隔 */
#define CONT_SILENCE_END_MS 1200 /* 有声后静音此时长 → 自动提交 */
#define CONT_SILENCE_ROLL_MS 5000 /* 纯静音：滚动清缓冲等开口（不打服务端） */
#define CONT_FLOOR_MARGIN    5    /* 高出底噪多少算有声 */
#define CONT_AMP_MIN         4    /* 绝对下限（滤数字底噪抖动） */
static int g_cont_floor;          /* 本轮底噪 EMA */

/* voice_session 状态（从 ui_voice 移植） */
static bool g_vs_inited;
static pthread_mutex_t g_voice_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_voice_done;
static char g_transcript[192];
static char g_reply[CONV_TEXT];
/* 采集缓冲 256KB：ESP 上 PSRAM 惰性分配（DRAM BSS 放不下），PC 上 malloc */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define G_CAPTURE_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#else
#include <stdlib.h>
#define G_CAPTURE_ALLOC(n) malloc((n))
#endif
#define G_CAPTURE_SAMPLES (16000 * 8)
static int16_t *g_capture;
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
    [UI_APP_CHAT]    = { "bigfish",    "AstrBot",    true  },
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

/* App 标识（与 Bridge 音色配置的 key 一致；未知返回 "chat"） */
static const char *conv_app_key(int app_id)
{
    const app_def_t *def = app_manager_find(app_id);
    return (def && def->key) ? def->key : "chat";
}

/* 统一线程创建。真机把线程栈放 PSRAM：
 * 绘制缓冲改到内部 RAM（DMA 零拷贝）后，内部 RAM 只剩 ~41KB，
 * 16KB 的语音线程栈 pthread_create 直接 ENOMEM（rc=12，真机实测：
 * 状态永远停在"思考中"）。PSRAM 栈需要
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y（已开）。 */
static int conv_thread_start(void *(*fn)(void *), void *arg, size_t stack)
{
#ifdef ESP_PLATFORM
    /* 内部 RAM 栈（PSRAM 栈实测引发 InstrFetchProhibited 栈损坏）；
     * 内部 RAM 由「字体不压缩 + WiFi 缓冲入 PSRAM」保证有余量 */
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    int rc = pthread_create(&tid, &attr, fn, arg);
    if (rc == 0) pthread_detach(tid);
    pthread_attr_destroy(&attr);
    return rc;
#else
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    int rc = pthread_create(&tid, &attr, fn, arg);
    if (rc == 0) pthread_detach(tid);
    pthread_attr_destroy(&attr);
    return rc;
#endif
}

static void voice_work_once(void)
{
    char transcript[192] = {0}, reply[CONV_TEXT] = {0};
    CLOG("voice worker start: app=%s frames=%d", conv_app_key(g_app_id),
         g_capture ? g_capture_frames : 0);
    ds_voice_interact(conv_app_key(g_app_id),
                      g_capture ? g_capture : (const int16_t *)"",
                      g_capture ? g_capture_frames : 0,
                      transcript, sizeof(transcript),
                      reply, sizeof(reply));
    CLOG("voice worker done: transcript=%u reply=%u", (unsigned)strlen(transcript),
         (unsigned)strlen(reply));
#ifdef ESP_PLATFORM
    CLOG("voice worker stack free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif

    /* 写入历史（transcript 作为用户气泡 + reply 作为助手气泡）。
     * chat App 按当前机器人实例分槽（多实例记录互不混） */
    {
        conv_msg_t *h = NULL;
        int *n = NULL;
        conv_hist_slot(g_app_id, &h, &n);
        if (h && n) {
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
    }

    pthread_mutex_lock(&g_voice_mu);
    strcpy(g_transcript, transcript);
    strcpy(g_reply, reply);
    g_voice_done = true;
    pthread_mutex_unlock(&g_voice_mu);
}

static void *voice_worker(void *arg)
{
    (void)arg;
    voice_work_once();
    return NULL;
}

/* 真机语音任务：常驻 + 静态栈（.bss），运行期不再向内部堆申请 12KB。
 * 背景（2026-11-15 真机反复踩）：聊天页打开后内部 RAM 被解码/网络切碎，
 * pthread 16KB→12KB 栈都拿不到（rc=12 ENOMEM），状态永远卡"思考中"；
 * 同时 SPI 的 19.2KB DMA bounce 也分配失败 → 花屏。静态栈 + 绘制缓冲
 * 开机预分配是唯一稳的组合。 */
#ifdef ESP_PLATFORM
#define VOICE_TASK_STACK 14336
static StaticTask_t s_voice_tcb;
static StackType_t s_voice_stack[VOICE_TASK_STACK / sizeof(StackType_t)];
static TaskHandle_t s_voice_task;
static volatile bool s_voice_busy;

static void voice_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_voice_busy = true;
        voice_work_once();
        s_voice_busy = false;
    }
}

static int voice_start(void)
{
    if (!s_voice_task) {
        s_voice_task = xTaskCreateStatic(voice_task_main, "voice",
                                         VOICE_TASK_STACK / sizeof(StackType_t),
                                         NULL, 5, s_voice_stack, &s_voice_tcb);
        if (!s_voice_task) return -1;
    }
    if (s_voice_busy) return -2; /* 上一回合还在跑：忽略重复触发 */
    xTaskNotifyGive(s_voice_task);
    return 0;
}
#endif

static void ptt_apply_state(vs_state_t st); /* 前向声明：on_vs_state 里要用 */

static void on_vs_state(vs_state_t st, const char *detail, void *user)
{
    (void)user;
    if (!g_status_lbl) return;
    /* 真机播放完成通知来自播放任务线程（vs_notify_play_done 链），
     * 回调内触碰 LVGL 必须持显示锁（ui.h；模拟器 no-op） */
    ui_lock();
    lv_label_set_text(g_status_lbl, state_text(st));
    companion_set_state(g_app_id, st);
    ptt_apply_state(st);
    ui_unlock();

    if (st == VS_LISTENING) g_capture_frames = 0;
    if (st == VS_THINKING) {
#ifdef ESP_PLATFORM
        int prc = voice_start(); /* 常驻静态栈任务，运行期零分配 */
#else
        int prc = conv_thread_start(voice_worker, NULL, 12 * 1024);
#endif
        CLOG("voice worker spawn rc=%d frames=%d", prc, g_capture_frames);
    }
}

static void on_vs_speak(const char *visible, void *user)
{
    (void)user;
    ui_lock();
    if (g_reply_lbl) lv_label_set_text(g_reply_lbl, visible);
    ui_unlock();
}

static void on_vs_pcm(const int16_t *pcm, int frames, void *user)
{
    (void)user;
    if (g_capture == NULL) {
        g_capture = G_CAPTURE_ALLOC(G_CAPTURE_SAMPLES * sizeof(int16_t));
        if (g_capture == NULL) return; /* 分配失败丢弃本帧 */
    }
    if (g_capture_frames + frames <= G_CAPTURE_SAMPLES) {
        memcpy(g_capture + g_capture_frames, pcm, (size_t)frames * 2);
        g_capture_frames += frames;
    }
    double sum = 0;
    for (int i = 0; i < frames; i++) sum += (double)pcm[i] * pcm[i];
    double rms = sqrt(sum / frames) / 32768.0;
    g_wave_amp = (int)(rms * 300);
    if (g_wave_amp > 100) g_wave_amp = 100;
    g_wave_data = true;
    /* 连续对话 VAD（自适应底噪 + 诊断幅度） */
    if (g_wave_amp > g_cont_maxamp) g_cont_maxamp = g_wave_amp;
    if (g_wave_amp >= g_cont_floor + CONT_FLOOR_MARGIN && g_wave_amp >= CONT_AMP_MIN) {
        g_cont_heard = true;
        g_cont_loud_tick = lv_tick_get();
    } else {
        g_cont_floor = (g_cont_floor * 7 + g_wave_amp) / 8;
    }
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

/* 历史改为按钮全屏查看后不再常驻渲染；保留空实现以兼容旧调用点 */
static void rerender_history(void)
{
}

/* PTT 按钮的四态视觉：idle=主色 / listening=红 / thinking=琥珀 /
 * speaking=绿。按住时另有 LV_STATE_PRESSED 样式（更亮 + 外圈），
 * 让"按住说话"有明确反馈（用户反馈：按下去按钮没变化）。 */
#define PTT_COL_IDLE     UI_ACCENT
#define PTT_COL_LISTEN   0xE5484D /* 录音中：红 */
#define PTT_COL_THINK    0xF5A623 /* 识别/思考：琥珀 */
#define PTT_COL_SPEAK    0x2EC4B6 /* 播放中：青 */

static void ptt_apply_state(vs_state_t st)
{
    if (!g_ptt_btn) return;
    lv_color_t c = UI_ACCENT;
    switch (st) {
    case VS_LISTENING: c = lv_color_hex(PTT_COL_LISTEN); break;
    case VS_THINKING:  c = lv_color_hex(PTT_COL_THINK);  break;
    case VS_SPEAKING:  c = lv_color_hex(PTT_COL_SPEAK);  break;
    case VS_ERROR:     c = lv_color_hex(PTT_COL_LISTEN); break;
    default: break;
    }
    lv_obj_set_style_bg_color(g_ptt_btn, c, 0);
    /* 录音中加一圈呼吸外环提示"正在录" */
    bool ring = (st == VS_LISTENING);
    lv_obj_set_style_border_width(g_ptt_btn, ring ? 4 : 0, 0);
    lv_obj_set_style_border_color(g_ptt_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(g_ptt_btn, ring ? LV_OPA_40 : LV_OPA_TRANSP, 0);
}

/* ---------------- PTT / poll ---------------- */

static void continuous_toggle_cb(lv_event_t *e);

static void continuous_toggle_cb_void(void)
{
    continuous_toggle_cb(NULL);
}

/* 自动轮没听到说话：放弃本轮（vs_cancel，不提交）并暂停连续模式 */
static void cont_pause_no_speech(void)
{
    g_continuous = false;
    g_cont_auto = false;
    if (vs_state() == VS_LISTENING) vs_cancel();
    if (g_cont_lbl) lv_obj_set_style_text_color(g_cont_lbl, UI_TEXT_DIM, 0);
    if (g_status_lbl) lv_label_set_text(g_status_lbl, "连续对话已暂停（没听到说话）");
}

static void continuous_toggle_cb(lv_event_t *e)
{
    (void)e;
    g_continuous = !g_continuous;
    if (!g_continuous && g_cont_auto && vs_state() == VS_LISTENING) {
        vs_cancel(); /* 关闭时正在自动听：放弃本轮（不提交不产文本） */
    }
    g_cont_auto = false;
    lv_obj_set_style_text_color(g_cont_lbl,
                                g_continuous ? UI_OK : UI_TEXT_DIM, 0);
    if (g_status_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s（连续对话%s）", state_text(VS_IDLE),
                 g_continuous ? "开" : "关");
        lv_label_set_text(g_status_lbl, buf);
    }
}

static void vol_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_statusbar_volume_popup();
}

static void ptt_pressed_cb(lv_event_t *e)
{
    (void)e;
    CLOG("ptt PRESSED voice_enabled=%d state=%d", (int)g_voice_enabled,
         (int)vs_state());
    g_cont_auto = false; /* 手动轮接管：VAD 不做自动提交 */
    if (g_voice_enabled) vs_ptt_begin();
    ptt_apply_state(VS_LISTENING);
}

/* KEY3 长按（Web 配置 ptt）：真机=按住期间说话，松开沿收口提交
 * （main key_task 分发 long_release）；模拟器无释放事件源，
 * 0.9s 定时器自动收尾（演示语义）。 */
static void key3_ptt_end_cb(void)
{
    if (g_voice_enabled) vs_ptt_end();
}

#ifndef ESP_PLATFORM
static void key3_ptt_auto_release_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    vs_ptt_end();
}
#endif

static void key3_long_ptt_cb(void)
{
    if (!g_voice_enabled) return;
    vs_ptt_begin();
#ifndef ESP_PLATFORM
    lv_timer_create(key3_ptt_auto_release_cb, 900, NULL);
#endif
}

static void ptt_released_cb(lv_event_t *e)
{
    (void)e;
    CLOG("ptt RELEASED state=%d", (int)vs_state());
    if (g_voice_enabled) vs_ptt_end();
}

/* PRESS_LOST：手指按住后被滚动/其他对象抢走焦点时 LVGL 不会补 RELEASED。
 * 原来只认 RELEASED → 录音永远收不了口（状态卡 Listening、无请求），
 * 表现就是"按住说话没反应"。丢失按抬处理：提交已录到的音频。 */
static void ptt_press_lost_cb(lv_event_t *e)
{
    (void)e;
    CLOG("ptt PRESS_LOST state=%d", (int)vs_state());
    if (g_voice_enabled) vs_ptt_end();
}

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* 录音看门狗：抬起沿丢失（PRESS_LOST 之外的路径）时不能让状态机
     * 永久停在 Listening——超过 20s 强制提交已录音频。 */
    static vs_state_t last_st;
    static uint32_t listen_t0, idle_since, cont_listen_t0;
    vs_state_t st = vs_state();
    if (st == VS_LISTENING && last_st != VS_LISTENING) listen_t0 = lv_tick_get();
    if (st == VS_IDLE && last_st != VS_IDLE) idle_since = lv_tick_get();
    last_st = st;
    if (st == VS_LISTENING && lv_tick_elaps(listen_t0) > 20000) {
        if (g_cont_auto && !g_cont_heard) {
            /* 自动轮 20s 没听到说话：放弃并暂停连续（绝不把静音发服务端
             * ——首版强提导致官方 ASR 对底噪幻觉出同一句，回复无限循环） */
            CLOG("continuous: 20s no speech (maxamp=%d floor=%d) — pause",
                 g_cont_maxamp, g_cont_floor);
            cont_pause_no_speech();
        } else {
            CLOG("ptt watchdog: force submit (20s)");
            vs_ptt_end();
        }
    }

    /* 连续对话（免提）驱动：
     *   IDLE（含回复播完）稍候自动开听；自动轮"说完停 1.2s"自动提交；
     *   纯静音不提交（滚动清缓冲等开口）；缓冲将满强制提交。
     *   手动按住轮（g_cont_auto=0）完全不受 VAD 干预。 */
    if (g_continuous && g_voice_enabled) {
        if (st == VS_IDLE && lv_tick_elaps(idle_since) > CONT_RESPEAK_GAP_MS) {
            g_cont_auto = true;
            g_cont_heard = false;
            g_cont_floor = 0;
            g_cont_maxamp = 0;
            cont_listen_t0 = lv_tick_get();
            CLOG("continuous: auto listen begin");
            vs_ptt_begin();
        } else if (st == VS_LISTENING && g_cont_auto) {
            if (g_cont_heard &&
                lv_tick_elaps(g_cont_loud_tick) > CONT_SILENCE_END_MS) {
                CLOG("continuous: silence %.0fms after speech (maxamp=%d) submit",
                     (double)lv_tick_elaps(g_cont_loud_tick), g_cont_maxamp);
                vs_ptt_end();
            } else if (!g_cont_heard &&
                       lv_tick_elaps(cont_listen_t0) > CONT_SILENCE_ROLL_MS) {
                /* 一直没人开口：清掉静音继续等（不发服务端）；
                 * maxamp 留痕用于校准阈值 */
                CLOG("continuous: roll silence (maxamp=%d floor=%d)",
                     g_cont_maxamp, g_cont_floor);
                g_capture_frames = 0;
                g_cont_maxamp = 0;
                cont_listen_t0 = lv_tick_get();
            } else if (g_capture_frames >= G_CAPTURE_SAMPLES - 1600) {
                CLOG("continuous: capture near full, force submit (maxamp=%d)",
                     g_cont_maxamp);
                vs_ptt_end();
            }
        }
    }

    vs_tick();

    pthread_mutex_lock(&g_voice_mu);
    bool done = g_voice_done;
    g_voice_done = false;
    char reply[CONV_TEXT];
    if (done) strcpy(reply, g_reply);
    pthread_mutex_unlock(&g_voice_mu);

    if (done) {
        /* 文本先落 UI/历史（流式时音频已在播，回复文本最后才到） */
        vs_set_reply(reply);
        if (g_reply_lbl) {
            char buf[CONV_TEXT + 16];
            snprintf(buf, sizeof(buf), "%s", reply[0] ? reply : g_transcript);
            lv_label_set_text(g_reply_lbl, buf);
        }
        if (vs_state() == VS_THINKING) {
            /* 非流式链路：等全文到齐才开播 */
            vs_notify_engine_done();
        }
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
            int side = i < WAVE_BARS_MAX / 2 ? -1 : 1;
            int idx = i < WAVE_BARS_MAX / 2 ? i : i - WAVE_BARS_MAX / 2;
            lv_obj_align(g_wave[i], LV_ALIGN_BOTTOM_MID,
                         side * (60 + idx * 9), -108);
            lv_obj_set_style_bg_opa(g_wave[i],
                                    vs_state() == VS_LISTENING ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

/* ---------------- 全屏历史模态 ---------------- */

static void hist_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    lv_obj_delete(mask);
}

static void hist_wipe_cb(lv_event_t *e);
static void hist_open_cb(lv_event_t *e)
{
    (void)e;
    if (g_app_id < 0) return;

    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mask, 480, 480);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list = lv_obj_create(mask);
    lv_obj_set_size(list, LV_PCT(100), 400);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    conv_msg_t *h = NULL;
    int *np = NULL;
    conv_hist_slot(g_app_id, &h, &np);
    if (!h || !np) { /* 防御：不该发生 */
        h = g_hist[g_app_id];
        np = &g_hist_n[g_app_id];
    }
    for (int i = 0; i < *np; i++) {
        add_bubble(list, h[i].role, h[i].text);
    }
    if (*np == 0) {
        lv_obj_t *empty = ui_label(list, "还没有对话记录", &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_center(empty);
    }
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);

    lv_obj_t *close = lv_button_create(mask);
    lv_obj_set_size(close, 140, 48);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 86, -14);
    ui_style_card(close);
    lv_obj_set_style_bg_color(close, UI_ACCENT, 0);
    lv_obj_set_style_radius(close, 16, 0);
    lv_obj_add_event_cb(close, hist_close_cb, LV_EVENT_CLICKED, mask);
    lv_obj_t *cl = ui_label(close, "关闭", &lv_font_cn_16, UI_TEXT);
    lv_obj_center(cl);

    lv_obj_t *wipe = lv_button_create(mask);
    lv_obj_set_size(wipe, 140, 48);
    lv_obj_align(wipe, LV_ALIGN_BOTTOM_MID, -86, -14);
    ui_style_card(wipe);
    lv_obj_set_style_radius(wipe, 16, 0);
    lv_obj_add_event_cb(wipe, hist_wipe_cb, LV_EVENT_CLICKED, mask);
    /* chat App：清记录 + 清服务端记忆（当前机器人）＝"新对话"；
     * 其余 App 只清本地记录（不碰 AstrBot 会话） */
    lv_obj_t *wl = ui_label(wipe, g_app_id == UI_APP_CHAT
                                ? LV_SYMBOL_REFRESH "  新对话"
                                : LV_SYMBOL_TRASH "  清空",
                            &lv_font_cn_16, UI_TEXT_DIM);
    lv_obj_center(wl);
}

/* "新对话"重置线程：HTTP 别卡 LVGL 线程（栈同 voice worker 预算）。
 * arg 指向静态缓冲 s_reset_bot：空串=全部实例；否则只清该机器人 */
static char s_reset_bot[64];
static void *chat_reset_worker(void *arg)
{
    (void)arg;
    char bot[64];
    snprintf(bot, sizeof(bot), "%s", s_reset_bot);
    int rc = ds_chat_reset(bot);
    CLOG("cloud session reset bot=%s rc=%d", bot, rc);
    return NULL;
}

/* ---- AstrBot 多实例：点标题弹"选择机器人"面板 ---- */
#define BOT_PICK_MAX 8
static char s_pick_id[BOT_PICK_MAX][64];
static char s_pick_label[BOT_PICK_MAX][32];
static int s_pick_n;
static lv_obj_t *g_picker_mask;

static void picker_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    if (mask) lv_obj_delete(mask);
    if (g_picker_mask == mask) g_picker_mask = NULL;
}

static void bot_thread_start(void *(*fn)(void *), void *arg);
static void bot_sync_start(void);

/* 选中某只机器人：后台 PUT（HTTP 不占 LVGL 任务），回来后改标题+关面板 */
static void *bot_pick_apply_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= s_pick_n) return NULL;
    char id[64], label[32];
    snprintf(id, sizeof(id), "%s", s_pick_id[idx]);
    snprintf(label, sizeof(label), "%s", s_pick_label[idx]);

    int rc = ds_chat_set_active(id);
    ui_lock();
    if (rc == 0) {
        snprintf(g_active_bot_id, sizeof(g_active_bot_id), "%s", id);
        snprintf(g_bot_label, sizeof(g_bot_label), "%s", label);
        if (g_title_lbl) lv_label_set_text(g_title_lbl, g_bot_label);
        if (g_status_lbl) lv_label_set_text(g_status_lbl, "已切换机器人");
    } else if (g_status_lbl) {
        lv_label_set_text(g_status_lbl, "切换失败（Bridge 未响应）");
    }
    if (g_picker_mask) {
        lv_obj_delete(g_picker_mask);
        g_picker_mask = NULL;
    }
    ui_unlock();
    return NULL;
}

static void picker_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bot_thread_start(bot_pick_apply_worker, (void *)(intptr_t)idx);
}

/* 建面板（调用方持显示锁）：列全部实例，当前项打勾。 */
static void bot_picker_build(const char *list, const char *cur_id)
{
    if (g_picker_mask) { /* 连点标题：旧面板先关掉，避免叠加 */
        lv_obj_delete(g_picker_mask);
        g_picker_mask = NULL;
    }
    s_pick_n = 0;
    static char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", list);
    char *save = NULL;
    for (char *line = strtok_r(tmp, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (s_pick_n >= BOT_PICK_MAX) break;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        snprintf(s_pick_id[s_pick_n], sizeof(s_pick_id[0]), "%s", line);
        snprintf(s_pick_label[s_pick_n], sizeof(s_pick_label[0]), "%s", tab + 1);
        s_pick_n++;
    }

    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mask, 480, 480);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    g_picker_mask = mask;

    lv_obj_t *title = ui_label(mask, "选择机器人", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *listbox = lv_obj_create(mask);
    lv_obj_set_size(listbox, 400, 330);
    lv_obj_align(listbox, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(listbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(listbox, 0, 0);
    lv_obj_set_style_pad_all(listbox, 4, 0);
    lv_obj_set_flex_flow(listbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listbox, 8, 0);

    for (int i = 0; i < s_pick_n; i++) {
        bool on = (cur_id && strcmp(s_pick_id[i], cur_id) == 0);
        lv_obj_t *btn = lv_button_create(listbox);
        lv_obj_set_size(btn, LV_PCT(100), 52);
        ui_style_card(btn);
        lv_obj_set_style_radius(btn, 16, 0);
        if (on) lv_obj_set_style_border_color(btn, UI_ACCENT, 0);
        lv_obj_add_event_cb(btn, picker_item_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s%.24s",
                 on ? LV_SYMBOL_OK "  " : "", s_pick_label[i]);
        lv_obj_t *l = ui_label(btn, buf, &lv_font_cn_16, on ? UI_ACCENT : UI_TEXT);
        lv_obj_center(l);
    }

    lv_obj_t *close = lv_button_create(mask);
    lv_obj_set_size(close, 160, 48);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -16);
    ui_style_card(close);
    lv_obj_set_style_radius(close, 16, 0);
    lv_obj_add_event_cb(close, picker_close_cb, LV_EVENT_CLICKED, mask);
    lv_obj_t *cl = ui_label(close, "关闭", &lv_font_cn_16, UI_TEXT);
    lv_obj_center(cl);
    CLOG("bot picker shown: %d bots", s_pick_n);
}

/* 点标题：后台拉列表+当前 → lv_async_call 回 LVGL 线程建面板。
 * 建 UI 必须在 LVGL 任务里做（跨线程持锁建对象曾在真机把显示管线
 * 卡死）；worker 只做 HTTP，栈给足（http_request 4KB + bots 4KB）。 */
static char s_pending_list[512];
static char s_pending_cur[64];

static void bot_picker_build_async(void *arg)
{
    (void)arg;
    bot_picker_build(s_pending_list, s_pending_cur);
}

static void *bot_picker_worker(void *arg)
{
    (void)arg;
    int rc = ds_chat_bots(s_pending_list, sizeof(s_pending_list));
    CLOG("bot picker fetch rc=%d n=%u", rc, (unsigned)strlen(s_pending_list));
#ifdef ESP_PLATFORM
    /* 真机栈用量留痕：http 链路 + 缓冲吃掉多少（12KB 曾溢出崩溃） */
    CLOG("picker worker stack free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    if (rc != 0 || s_pending_list[0] == '\0') {
        ui_lock();
        if (g_status_lbl) lv_label_set_text(g_status_lbl, "没有 AstrBot 机器人实例");
        ui_unlock();
        return NULL;
    }
    s_pending_cur[0] = '\0';
    (void)ds_chat_active_id(s_pending_cur, sizeof(s_pending_cur));
    ui_lock();
    lv_async_call(bot_picker_build_async, NULL);
    ui_unlock();
    return NULL;
}

/* 进页面时同步一次"当前机器人"名称到标题（不切换）。 */
static void *bot_sync_worker(void *arg)
{
    (void)arg;
    char cur[64] = {0};
    if (ds_chat_active_id(cur, sizeof(cur)) != 0 || cur[0] == '\0') return NULL;
    snprintf(g_active_bot_id, sizeof(g_active_bot_id), "%s", cur);

    char list[512];
    if (ds_chat_bots(list, sizeof(list)) != 0) return NULL;
    char tmp[sizeof(list)];
    snprintf(tmp, sizeof(tmp), "%s", list);
    char *save = NULL;
    for (char *line = strtok_r(tmp, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (strcmp(line, cur) == 0) {
            ui_lock();
            snprintf(g_bot_label, sizeof(g_bot_label), "%s", tab + 1);
            if (g_title_lbl) lv_label_set_text(g_title_lbl, g_bot_label);
            ui_unlock();
            return NULL;
        }
    }
    return NULL;
}

static void bot_thread_start(void *(*fn)(void *), void *arg)
{
    /* 栈需求实测：http_request 4KB + 调用方 4KB（bots）+ 1KB 请求行
     * + cJSON；http 读缓冲已改堆分配后 16KB 足够（原 24KB） */
    (void)conv_thread_start(fn, arg, 16 * 1024);
}

static void bot_sync_start(void) { bot_thread_start(bot_sync_worker, NULL); }

/* 自检入口（临时构建用：真机无法手动点标题时驱动面板） */
static void bot_picker_start(void);
void ui_conversation_debug_bot_picker(void) { bot_picker_start(); }
static void bot_picker_start(void) { bot_thread_start(bot_picker_worker, NULL); }

static void title_pick_cb(lv_event_t *e)
{
    (void)e;
    bot_picker_start();
}

/* 历史弹窗底部按钮：chat App="新对话"（清当前机器人本地气泡 + 服务端
 * 记忆——Bridge 轮换该实例 username/session）；其余 App="清空"（只清
 * 本地气泡，不碰 AstrBot 会话——之前这里无差别 reset，把 Muse 小智的
 * 清空也变成了重置全部机器人，顺手修掉）。 */
static void hist_wipe_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    int id = g_app_id;
    bool is_chat = (id == UI_APP_CHAT);

    conv_msg_t *h = NULL;
    int *np = NULL;
    conv_hist_slot(id, &h, &np);
    if (h && np) {
        *np = 0;
        size_t bytes = (size_t)CONV_MSGS * sizeof(conv_msg_t);
        if (h >= s_bot_hist &&
            h < s_bot_hist + (size_t)BOT_HIST_BOTS * CONV_MSGS) {
            memset(h, 0, bytes); /* PSRAM 分槽：只清本槽 */
            for (int i = 0; i < BOT_HIST_BOTS; i++) {
                if (&s_bot_hist[(size_t)i * CONV_MSGS] == h) {
                    s_bot_hist_id[i][0] = '\0'; /* 槽位可复用 */
                }
            }
        } else if (id >= 0 && id < UI_APP_COUNT) {
            memset(g_hist[id], 0, bytes);
        }
    }
    if (mask) lv_obj_delete(mask); /* 直接销毁弹窗（hist_close_cb 走 event 取参，不复用） */
    if (g_reply_lbl) lv_label_set_text(g_reply_lbl, "");

    if (is_chat) {
        snprintf(s_reset_bot, sizeof(s_reset_bot), "%s", g_active_bot_id);
        (void)conv_thread_start(chat_reset_worker, NULL, 16 * 1024);
    } else {
        if (g_status_lbl) lv_label_set_text(g_status_lbl, "已清空");
    }
}

/* ---------------- 页面构建 ---------------- */

void ui_conversation_build(lv_obj_t *scr, int app_id)
{
    g_app_id = app_id;
    g_voice_enabled = g_meta[app_id].voice;
    ui_key_set_key3_handler(continuous_toggle_cb_void); /* KEY3 短按：连续对话 */
    ui_key_set_key3_long_handler(key3_long_ptt_cb);     /* KEY3 长按：语音说话 */
    ui_key_set_key3_long_release_handler(key3_ptt_end_cb); /* 真机松开：收口提交 */

    if (!g_vs_inited) {
#ifdef ESP_PLATFORM
        /* 真机：ES7210 采集 / ES8311 播 TTS（audio_esp32） */
        vs_init(audio_esp32_ops());
#else
        /* 模拟器：mock ops + 伪采集波形 */
        vs_init(audio_mock_ops());
        vs_set_mock_capture(true);
#endif
        vs_set_listener(&g_listener);
        g_vs_inited = true;
    }
    if (!vs_acquire(g_meta[app_id].engine)) {
        lv_obj_t *busy = ui_label(scr, "语音被其他引擎占用", &lv_font_cn_16, UI_ERR);
        lv_obj_center(busy);
        return;
    }

    /* 顶栏：头像 + 标题 + 在线状态（定稿图对齐） */
    static const char *avatar_paths[UI_APP_COUNT] = {
        [UI_APP_CHAT]    = "A:simulator/lvgl-pc/assets/avatars/chat.png",
        [UI_APP_MUSE]    = "A:simulator/lvgl-pc/assets/avatars/muse.png",
        [UI_APP_XIAOZHI] = "A:simulator/lvgl-pc/assets/avatars/xiaozhi.png",
    };
    if (avatar_paths[app_id]) {
        lv_obj_t *avatar = lv_image_create(scr);
        lv_image_set_src(avatar, avatar_paths[app_id]);
        lv_obj_set_size(avatar, 44, 44);
        lv_obj_align(avatar, LV_ALIGN_TOP_LEFT, 64, 0);
        lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(avatar, true, 0);
    }

    lv_obj_t *online_dot = lv_obj_create(scr);
    lv_obj_set_size(online_dot, 8, 8);
    lv_obj_set_style_radius(online_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(online_dot, UI_OK, 0);
    lv_obj_set_style_bg_opa(online_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(online_dot, 0, 0);
    lv_obj_align(online_dot, LV_ALIGN_TOP_MID, -28, 38);
    lv_obj_t *online = ui_label(scr, "在线", &lv_font_cn_12, UI_OK);
    lv_obj_align(online, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *title_lbl = ui_label(scr, g_meta[app_id].title, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 10);
    g_title_lbl = title_lbl;
    if (app_id == UI_APP_CHAT) {
        /* AstrBot：标题显示当前机器人，点它弹出"选择机器人"面板
         * （多实例显式选择；切换值落 Bridge，语音/文本都跟随，重启保持） */
        if (g_bot_label[0]) lv_label_set_text(title_lbl, g_bot_label);
        lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(title_lbl, title_pick_cb, LV_EVENT_CLICKED, NULL);
        bot_sync_start(); /* 拉一次当前机器人名（异步，不阻塞首帧） */
    }

    ui_gesture_back_add(scr); /* 上滑退出（无返回按钮，ADR-008 范式） */

    /* 连续对话开关（设备端 KEY3 短按同语义；模拟器按钮呈现） */
    lv_obj_t *cont = lv_button_create(scr);
    lv_obj_set_size(cont, 56, 44);
    lv_obj_align(cont, LV_ALIGN_TOP_LEFT, 0, 28);
    ui_style_card(cont);
    lv_obj_set_style_radius(cont, 14, 0);
    lv_obj_add_event_cb(cont, continuous_toggle_cb, LV_EVENT_CLICKED, NULL);
    g_cont_lbl = ui_label(cont, LV_SYMBOL_LOOP, &lv_font_montserrat_16,
                          g_continuous ? UI_OK : UI_TEXT_DIM);
    lv_obj_center(g_cont_lbl);

    /* 历史不直接展示：顶部"历史"按钮 → 全屏滚动查看
     * （y28：避开全局状态栏 overlay，PLAN §13.3） */
    lv_obj_t *hist_btn = lv_button_create(scr);
    lv_obj_set_size(hist_btn, 56, 44);
    lv_obj_align(hist_btn, LV_ALIGN_TOP_RIGHT, 0, 28);
    ui_style_card(hist_btn);
    lv_obj_set_style_radius(hist_btn, 14, 0);
    lv_obj_add_event_cb(hist_btn, hist_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hicon = ui_label(hist_btn, LV_SYMBOL_LIST, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(hicon);

    /* 快速音量：复用状态栏弹窗（含扬声器实时生效） */
    lv_obj_t *vol_btn = lv_button_create(scr);
    lv_obj_set_size(vol_btn, 56, 44);
    lv_obj_align(vol_btn, LV_ALIGN_TOP_RIGHT, -64, 28);
    ui_style_card(vol_btn);
    lv_obj_set_style_radius(vol_btn, 14, 0);
    lv_obj_add_event_cb(vol_btn, vol_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vicon = ui_label(vol_btn, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(vicon);

    companion_create(scr, app_id, 80);

    /* headless 验收钩子：GADGET_DEMO_BOT_PICK=1 进 AstrBot 页即弹
     * 机器人选择面板（快照回归用；真机无此环境变量） */
    if (app_id == UI_APP_CHAT && getenv("GADGET_DEMO_BOT_PICK") &&
        getenv("GADGET_DEMO_BOT_PICK")[0] == '1') {
        bot_picker_start();
    }

    g_status_lbl = ui_label(scr, state_text(VS_IDLE), &lv_font_cn_14, UI_TEXT);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_MID, 0, 262);

    g_reply_lbl = ui_label(scr, "", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(g_reply_lbl, LV_ALIGN_TOP_MID, 0, 284);
    lv_label_set_long_mode(g_reply_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_reply_lbl, 440);
    lv_label_set_long_mode(g_reply_lbl, LV_LABEL_LONG_DOT);

    /* 波形：麦克风两侧（左 12 / 右 12，定稿图对齐） */
    for (int i = 0; i < WAVE_BARS_MAX; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_set_size(bar, 6, 8);
        lv_obj_set_style_bg_color(bar, UI_ACCENT2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        int side = i < WAVE_BARS_MAX / 2 ? -1 : 1;
        int idx = i < WAVE_BARS_MAX / 2 ? i : i - WAVE_BARS_MAX / 2;
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID,
                     side * (60 + idx * 9), -108);
        g_wave[i] = bar;
    }

    /* 底部交互区（定稿图）：中央麦克风圆按钮（按住说话）+ 左右波形 */
    g_ptt_btn = lv_button_create(scr);
    lv_obj_set_size(g_ptt_btn, 96, 96);
    lv_obj_align(g_ptt_btn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(g_ptt_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_ptt_btn, g_voice_enabled ? UI_ACCENT : UI_CARD, 0);
    lv_obj_set_style_bg_opa(g_ptt_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_ptt_btn, 0, 0);
    lv_obj_set_style_shadow_width(g_ptt_btn, 0, 0);
    /* 按住反馈：按下时提亮 + 内缩，抬起恢复（否则按住期间毫无变化） */
    lv_obj_set_style_bg_color(g_ptt_btn, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g_ptt_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(g_ptt_btn, 236, LV_STATE_PRESSED); /* ~0.92 */
    lv_obj_set_style_transform_scale(g_ptt_btn, 256, 0);
    if (g_voice_enabled) {
        lv_obj_add_event_cb(g_ptt_btn, ptt_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(g_ptt_btn, ptt_released_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(g_ptt_btn, ptt_press_lost_cb, LV_EVENT_PRESS_LOST,
                            NULL);
    }
    lv_obj_t *mic = ui_label(g_ptt_btn, LV_SYMBOL_AUDIO, &lv_font_montserrat_28,
                             g_voice_enabled ? UI_TEXT : UI_TEXT_DIM);
    lv_obj_center(mic);
    if (g_voice_enabled) ptt_apply_state(vs_state());

    if (!g_voice_enabled) {
        lv_obj_t *hint = ui_label(scr, "语音对话 · 等待数据源（PLAN §5.7）",
                                  &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 6);
    }

    if (!g_voice_enabled) {
        lv_obj_t *hint = ui_label(scr, "ZCode 数据源待接入（PLAN §5.7）",
                                  &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 262);
        lv_obj_set_style_text_color(hint, UI_TEXT_DIM, 0);
        lv_obj_del(g_status_lbl);
        g_status_lbl = NULL;
    }

    g_poll_timer = lv_timer_create(poll_timer_cb, 80, NULL); /* 12.5Hz：波形重绘降载 */
}

void ui_conversation_on_exit(void)
{
    if (g_poll_timer) {
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }
    vs_release();
    g_cont_auto = false;
    g_status_lbl = NULL;
    g_reply_lbl = NULL;
    g_title_lbl = NULL; /* 后台 worker 可能晚到（切机器人/语音），置空防悬垂 */
    g_ptt_btn = NULL;
    companion_delete();
    for (int i = 0; i < WAVE_BARS_MAX; i++) g_wave[i] = NULL;
    g_wave_data = false;
    g_app_id = -1;
}
