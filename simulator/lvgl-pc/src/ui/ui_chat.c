/* ui_chat — Bigfish Chat 设备端 UI（PLAN §5.3）：
 * 气泡流 + 输入框 + 发送。
 * 网络在后台线程执行（pthread），UI 定时器只轮询结果 —— 渲染永不阻塞。
 * 会话历史存静态缓冲，退出 App 再进历史仍在（上限 16 条，FIFO 淘汰）。
 * Phase 5 简化：非流式回复（一次性显示）；SSE 流式 TODO。
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_theme.h"

#define CHAT_MAX_MSGS  16
#define CHAT_MAX_TEXT  512

typedef struct {
    char role[12];
    char text[CHAT_MAX_TEXT];
} chat_msg_t;

static chat_msg_t g_history[CHAT_MAX_MSGS];
static int g_count;

/* 后台请求状态（g_state_mu 保护；UI 线程只轮询 flag） */
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_req_in_flight;
static bool g_req_done;
static char g_pending_reply[CHAT_MAX_TEXT];
static char g_stream_buf[CHAT_MAX_TEXT]; /* 流式增量累积（worker 写） */
static size_t g_stream_flushed;          /* UI 已渲染到的位置 */
static ds_chat_msg_t g_req_msgs[CHAT_MAX_MSGS]; /* 深拷贝，线程安全 */
static char g_req_text[CHAT_MAX_MSGS][CHAT_MAX_TEXT];
static int g_req_n;

static lv_obj_t *g_list;
static lv_obj_t *g_input;
static lv_obj_t *g_send_btn_lbl;
static lv_obj_t *g_poll_timer;

/* ---------------- 气泡渲染 ---------------- */

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
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = ui_label(bubble, text, &lv_font_cn_14, UI_TEXT);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(bubble, 340, 0);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
}

static void rerender_history(lv_obj_t *list)
{
    lv_obj_clean(list);
    for (int i = 0; i < g_count; i++) {
        add_bubble(list, g_history[i].role, g_history[i].text);
    }
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
}

/* ---------------- 后台发送线程 ---------------- */

/* worker：delta 回调里只往缓冲追加（LVGL 非线程安全，绝不碰 UI） */
static void stream_on_delta(const char *delta, void *user)
{
    (void)user;
    pthread_mutex_lock(&g_state_mu);
    size_t len = strlen(g_stream_buf);
    snprintf(g_stream_buf + len, sizeof(g_stream_buf) - len, "%s", delta);
    pthread_mutex_unlock(&g_state_mu);
}

static void *chat_worker(void *arg)
{
    (void)arg;
    char reply[CHAT_MAX_TEXT] = {0};
    ds_chat_result_t r = ds_chat_send_stream(g_req_msgs, g_req_n,
                                             stream_on_delta, NULL,
                                             reply, sizeof(reply));
    if (r != DS_CHAT_OK && reply[0] == '\0') {
        strcpy(reply, "(请求失败)");
    }

    pthread_mutex_lock(&g_state_mu);
    strcpy(g_pending_reply, reply);
    g_req_done = true;
    g_req_in_flight = false;
    pthread_mutex_unlock(&g_state_mu);
    return NULL;
}

static void start_request(void)
{
    /* 深拷贝历史（不含尾部 pending 占位） */
    pthread_mutex_lock(&g_state_mu);
    g_req_n = 0;
    for (int i = 0; i < g_count && g_req_n < CHAT_MAX_MSGS; i++) {
        if (strcmp(g_history[i].role, "pending") == 0) break;
        strcpy(g_req_text[g_req_n], g_history[i].text);
        g_req_msgs[g_req_n].role = g_history[i].role;
        g_req_msgs[g_req_n].content = g_req_text[g_req_n];
        g_req_n++;
    }
    g_req_done = false;
    g_req_in_flight = true;
    g_stream_buf[0] = '\0';
    g_stream_flushed = 0;
    pthread_mutex_unlock(&g_state_mu);

    pthread_t tid;
    pthread_create(&tid, NULL, chat_worker, NULL);
    pthread_detach(tid);
}

/* ---------------- 轮询定时器 ---------------- */

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    char newpart[CHAT_MAX_TEXT];
    bool done = false;
    char done_reply[CHAT_MAX_TEXT] = {0};

    pthread_mutex_lock(&g_state_mu);
    size_t blen = strlen(g_stream_buf);
    if (blen > g_stream_flushed) {
        snprintf(newpart, sizeof(newpart), "%s", g_stream_buf + g_stream_flushed);
        g_stream_flushed = blen;
    } else {
        newpart[0] = '\0';
    }
    done = g_req_done;
    if (done) strcpy(done_reply, g_pending_reply);
    pthread_mutex_unlock(&g_state_mu);

    if (g_count > 0 && strcmp(g_history[g_count - 1].role, "pending") == 0) {
        if (newpart[0]) {
            /* 流式增量：追加到占位气泡并刷新（LVGL 9.2 无 LV_LABEL_LAST，
             * 读全文拼接再回写；文本量小可接受） */
            lv_obj_t *last_row = lv_obj_get_child(g_list, lv_obj_get_child_cnt(g_list) - 1);
            lv_obj_t *bubble = lv_obj_get_child(last_row, 0);
            lv_obj_t *lbl = lv_obj_get_child(bubble, 0);
            char cur[CHAT_MAX_TEXT * 2];
            snprintf(cur, sizeof(cur), "%s%s", lv_label_get_text(lbl), newpart);
            lv_label_set_text(lbl, cur);
            lv_obj_scroll_to_y(g_list, LV_COORD_MAX, LV_ANIM_OFF);
        }
        if (done) {
            /* 流结束：落库历史（用完整回复，防截断不一致） */
            snprintf(g_history[g_count - 1].role,
                     sizeof(g_history[g_count - 1].role), "assistant");
            snprintf(g_history[g_count - 1].text,
                     sizeof(g_history[g_count - 1].text), "%s",
                     done_reply[0] ? done_reply : "(无回复)");
            if (g_send_btn_lbl) lv_label_set_text(g_send_btn_lbl, LV_SYMBOL_RIGHT);
            rerender_history(g_list);
        }
    } else if (done) {
        if (g_send_btn_lbl) lv_label_set_text(g_send_btn_lbl, LV_SYMBOL_RIGHT);
    }
}

/* ---------------- 发送 ---------------- */

static void send_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!g_input) return;
    const char *text = lv_textarea_get_text(g_input);
    if (!text[0]) return;

    /* FIFO 淘汰：满 16 条时去掉最旧一对 */
    if (g_count >= CHAT_MAX_MSGS - 1) {
        memmove(&g_history[0], &g_history[2], sizeof(chat_msg_t) * (g_count - 2));
        g_count -= 2;
    }

    snprintf(g_history[g_count].role, sizeof(g_history[g_count].role), "user");
    snprintf(g_history[g_count].text, sizeof(g_history[g_count].text), "%.127s", text);
    g_count++;
    strcpy(g_history[g_count].role, "pending");
    strcpy(g_history[g_count].text, "…");
    g_count++;

    lv_textarea_set_text(g_input, "");
    if (g_list) rerender_history(g_list);
    if (g_send_btn_lbl) lv_label_set_text(g_send_btn_lbl, LV_SYMBOL_REFRESH);

    start_request();
}

/* ---------------- 页面构建 ---------------- */

void ui_chat_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    lv_obj_t *title = ui_label(scr, "大肥鱼聊天", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);

    g_list = lv_obj_create(scr);
    lv_obj_set_size(g_list, LV_PCT(100), 320);
    lv_obj_align(g_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_all(g_list, 4, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_list, 8, 0);

    lv_obj_t *input_row = lv_obj_create(scr);
    lv_obj_set_size(input_row, LV_PCT(100), 52);
    lv_obj_align(input_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, 8, 0);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    g_input = lv_textarea_create(input_row);
    lv_obj_set_size(g_input, 330, 48);
    lv_textarea_set_placeholder_text(g_input, "输入消息…");
    lv_obj_set_style_bg_color(g_input, UI_CARD, 0);
    lv_obj_set_style_border_color(g_input, UI_LINE, 0);
    lv_obj_set_style_radius(g_input, 14, 0);
    lv_obj_set_style_text_font(g_input, &lv_font_cn_14, 0);

    lv_obj_t *send = lv_button_create(input_row);
    lv_obj_set_size(send, 64, 48);
    ui_style_card(send);
    lv_obj_set_style_bg_color(send, UI_ACCENT, 0);
    lv_obj_set_style_radius(send, 14, 0);
    lv_obj_add_event_cb(send, send_btn_cb, LV_EVENT_CLICKED, NULL);
    g_send_btn_lbl = ui_label(send, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(g_send_btn_lbl);

    rerender_history(g_list);

    /* 结果轮询（页面销毁时由 on_exit 停止） */
    g_poll_timer = lv_timer_create(poll_timer_cb, 100, NULL);
}

void ui_chat_on_exit(void)
{
    if (g_poll_timer) {
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }
    g_list = NULL;
    g_input = NULL;
    g_send_btn_lbl = NULL;
}
