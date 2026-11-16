/* ui_files — 文件管理器（ADR-010，2026-11-16）：管理 TF 卡目录/文件。
 *
 * 能力：浏览目录（目录优先排序）/ 返回上级 / 删除文件（两击确认）/
 * 上传到 Bridge / 查看 Bridge 文件并下载到 TF downloads/。
 * 线程纪律：目录扫描、HTTP、文件读写全部在 worker（16KB pthread）里跑，
 * 完成后 lv_async_call 回 LVGL 线程渲染——LVGL 线程零阻塞 IO（踩坑 #11）。
 * 页面代际 g_gen：退出/重建时自增，在途 worker 的回调据此丢弃防悬垂。
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_theme.h"

#define FILES_MAX 40

/* ---- 页面状态 ---- */
static int g_app_id = -1;
static char g_dir[192];  /* 当前目录绝对路径 */
static bool g_remote;    /* true = Bridge 文件视图 */
static int g_gen;        /* 页面代际（on_exit/退出时 ++） */
static volatile bool g_busy;

/* worker 产出（写线程→读线程一次，靠 lv_async_call 时序保证可见性） */
static ds_file_entry_t g_entries[FILES_MAX];
static int g_entries_n;
static int g_list_err;
static char g_remote_txt[2048]; /* 远端列表 "name\tsize" 行 */
static char g_remote_names[FILES_MAX][DS_FILE_NAME_MAX];
static long g_remote_sizes[FILES_MAX];
static int g_remote_n;
static char g_msg[160];

/* LVGL 引用（页面销毁后不触碰；每次 build 重置） */
static lv_obj_t *g_list;
static lv_obj_t *g_path_lbl;
static lv_obj_t *g_msg_lbl;
static lv_obj_t *g_modal;
static char g_sel_path[192];

/* ---- 工具 ---- */

static void fmt_size(char *out, size_t cap, long v)
{
    if (v < 1024) snprintf(out, cap, "%ld B", v);
    else if (v < 1024 * 1024) snprintf(out, cap, "%.1f KB", (double)v / 1024.0);
    else snprintf(out, cap, "%.1f MB", (double)v / (1024.0 * 1024.0));
}

static void toast(const char *s)
{
    /* s 可能就是 g_msg 本身（worker 已填好结果）——避免 snprintf 自覆盖 */
    if (s && s != g_msg) {
        snprintf(g_msg, sizeof(g_msg), "%s", s);
    }
    if (g_msg_lbl) {
        lv_label_set_text(g_msg_lbl, g_msg);
        lv_obj_clear_flag(g_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 上级目录（不超过 TF 根） */
static void dir_up(void)
{
    const char *root = ds_sd_root();
    size_t rl = strlen(root);
    char *slash = strrchr(g_dir, '/');
    if (!slash || (size_t)(slash - g_dir) < rl) return;
    *slash = '\0';
    if (strlen(g_dir) < rl) snprintf(g_dir, sizeof(g_dir), "%s", root);
}

static void files_reopen(void)
{
    if (g_app_id >= 0) app_manager_open(g_app_id);
}

/* ---- worker 基础设施 ---- */

static void files_thread(void *(*fn)(void *), void *arg)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024); /* HTTP 链 + cJSON，同 wx_worker */
    if (pthread_create(&tid, &attr, fn, arg) == 0) {
        pthread_attr_destroy(&attr);
        pthread_detach(tid);
    } else {
        pthread_attr_destroy(&attr);
        g_busy = false;
    }
}

/* ---- 列表 ---- */

static void render_rows(void);

static void list_async(void *arg)
{
    g_busy = false;
    if ((int)(intptr_t)arg != g_gen) return; /* 页面已退出/重建：丢弃 */
    if (g_path_lbl) lv_label_set_text(g_path_lbl, g_remote ? "Bridge 文件" : g_dir);
    render_rows();
}

static void *list_worker(void *arg)
{
    int gen = (int)(intptr_t)arg;
    if (g_remote) {
        g_remote_n = 0;
        g_remote_txt[0] = '\0';
        int rc = ds_files_remote_list(g_remote_txt, sizeof(g_remote_txt));
        g_list_err = rc ? -1 : 0;
        if (rc == 0) {
            /* "name\tsize" 行 → 数组（在 worker 里解析，UI 线程零字符串活） */
            char *save = NULL;
            char *line = strtok_r(g_remote_txt, "\n", &save);
            while (line && g_remote_n < FILES_MAX) {
                char *tab = strchr(line, '\t');
                if (tab) {
                    *tab = '\0';
                    g_remote_sizes[g_remote_n] = atol(tab + 1);
                } else {
                    g_remote_sizes[g_remote_n] = 0;
                }
                snprintf(g_remote_names[g_remote_n], DS_FILE_NAME_MAX, "%s", line);
                g_remote_n++;
                line = strtok_r(NULL, "\n", &save);
            }
        }
    } else {
        g_list_err = ds_files_list(g_dir, g_entries, FILES_MAX, &g_entries_n);
    }
    lv_async_call(list_async, (void *)(intptr_t)gen);
    return NULL;
}

/* 行：符号 + 名称 + 右侧信息；user_data=行下标（-1=上级） */
static void row_add(lv_obj_t *parent, const char *symbol, const char *name,
                    const char *right, int idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 46);
    ui_style_card(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *sym = ui_label(row, symbol, &lv_font_montserrat_16, UI_ACCENT);
    lv_obj_align(sym, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *nm = ui_label(row, name, &lv_font_cn_14, UI_TEXT);
    lv_obj_set_width(nm, 280);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 38, 0);

    if (right && right[0]) {
        lv_obj_t *rt = ui_label(row, right, &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(rt, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    (void)idx; /* 回调由调用方按类型挂（本地/远端） */
}

/* 点击行：需在 render 后统一挂回调（lv_event_cb_t 需要真实函数） */
static void file_row_cb(lv_event_t *e);
static void remote_row_cb(lv_event_t *e);

static void render_rows(void)
{
    if (!g_list) return;
    lv_obj_clean(g_list);

    char sz[24];
    if (g_list_err != 0) {
        lv_obj_t *l = ui_label(g_list, g_remote ? "Bridge 不可达（检查网络/地址）"
                                                : "未插卡或无法访问 TF 卡",
                               &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_center(l);
        return;
    }

    if (g_remote) {
        if (g_remote_n == 0) {
            lv_obj_t *l = ui_label(g_list, "Bridge 暂无文件（Web「设备文件」页上传）",
                                   &lv_font_cn_14, UI_TEXT_DIM);
            lv_obj_center(l);
            return;
        }
        for (int i = 0; i < g_remote_n; i++) {
            fmt_size(sz, sizeof(sz), g_remote_sizes[i]);
            row_add(g_list, LV_SYMBOL_DOWNLOAD, g_remote_names[i], sz, i);
            lv_obj_t *row = lv_obj_get_child(g_list, lv_obj_get_child_count(g_list) - 1);
            lv_obj_add_event_cb(row, remote_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        return;
    }

    /* 本地：非根目录首行=上级 */
    if (strcmp(g_dir, ds_sd_root()) != 0) {
        row_add(g_list, LV_SYMBOL_UP, "..", "", -1);
        lv_obj_t *row = lv_obj_get_child(g_list, lv_obj_get_child_count(g_list) - 1);
        lv_obj_add_event_cb(row, file_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    }
    for (int i = 0; i < g_entries_n; i++) {
        if (g_entries[i].is_dir) {
            row_add(g_list, LV_SYMBOL_DIRECTORY, g_entries[i].name, "", i);
        } else {
            fmt_size(sz, sizeof(sz), g_entries[i].size);
            row_add(g_list, LV_SYMBOL_FILE, g_entries[i].name, sz, i);
        }
        lv_obj_t *row = lv_obj_get_child(g_list, lv_obj_get_child_count(g_list) - 1);
        lv_obj_add_event_cb(row, file_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void list_start(void)
{
    if (g_busy) return;
    g_busy = true;
    files_thread(list_worker, (void *)(intptr_t)g_gen);
}

/* ---- 文件操作 modal ---- */

static void modal_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_modal) {
        lv_obj_delete(g_modal);
        g_modal = NULL;
    }
}

/* 上传/删除完成回调（LVGL 线程） */
static void op_done_async(void *arg)
{
    (void)arg;
    g_busy = false;
    toast(g_msg);
    if (g_modal) {
        lv_obj_delete(g_modal);
        g_modal = NULL;
    }
    files_reopen(); /* 列表/状态刷新（删除后文件消失；上传后无变化但统一） */
}

static void *op_worker_upload(void *arg)
{
    ds_files_upload(g_sel_path, g_msg, sizeof(g_msg));
    lv_async_call(op_done_async, arg);
    return NULL;
}

static void *op_worker_delete(void *arg)
{
    if (ds_files_delete(g_sel_path) == 0) {
        snprintf(g_msg, sizeof(g_msg), "已删除 %s", strrchr(g_sel_path, '/') + 1);
    } else {
        snprintf(g_msg, sizeof(g_msg), "删除失败");
    }
    lv_async_call(op_done_async, arg);
    return NULL;
}

static void *op_worker_download(void *arg)
{
    const char *name = (const char *)arg;
    char msg[120];
    if (ds_files_download(name, msg, sizeof(msg)) == 0) {
        snprintf(g_msg, sizeof(g_msg), "%s → downloads/", msg);
        g_remote = false;
        snprintf(g_dir, sizeof(g_dir), "%s/downloads", ds_sd_root());
    } else {
        snprintf(g_msg, sizeof(g_msg), "%s", msg);
    }
    lv_async_call(op_done_async, NULL);
    return NULL;
}

static void modal_upload_cb(lv_event_t *e)
{
    (void)e;
    if (g_busy) return;
    g_busy = true;
    files_thread(op_worker_upload, (void *)(intptr_t)g_gen);
}

static void modal_del_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    /* 两击确认：第一次只把按钮文案改成"确认删除" */
    if (strcmp(lv_label_get_text(lbl), "确认删除") != 0) {
        lv_label_set_text(lbl, "确认删除");
        lv_obj_set_style_bg_color(btn, UI_ERR, 0);
        return;
    }
    if (g_busy) return;
    g_busy = true;
    files_thread(op_worker_delete, (void *)(intptr_t)g_gen);
}

static void modal_open(const char *path, const char *name)
{
    if (g_modal) return;
    snprintf(g_sel_path, sizeof(g_sel_path), "%s", path);

    g_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_modal, 0, 0);
    lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_modal);
    lv_obj_set_size(card, 340, 236);
    lv_obj_center(card);
    ui_style_card(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = ui_label(card, name, &lv_font_cn_16, UI_TEXT);
    lv_obj_set_width(t, 300);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    struct {
        const char *txt;
        lv_event_cb_t cb;
    } btns[] = {
        { "上传到 Bridge", modal_upload_cb },
        { "删除", modal_del_cb },
        { "关闭", modal_close_cb },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, 280, 44);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 44 + i * 52);
        if (strcmp(btns[i].txt, "删除") == 0) {
            lv_obj_set_style_bg_color(b, UI_ERR, 0);
        }
        ui_label(b, btns[i].txt, &lv_font_cn_14, UI_TEXT);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
    }
}

/* ---- 行点击 ---- */

static void file_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == -1) {
        dir_up();
        files_reopen();
        return;
    }
    if (idx < 0 || idx >= g_entries_n) return;
    char path[192];
    if (snprintf(path, sizeof(path), "%s/%s", g_dir, g_entries[idx].name) >= (int)sizeof(path)) {
        return; /* 路径过长 */
    }
    if (g_entries[idx].is_dir) {
        snprintf(g_dir, sizeof(g_dir), "%s", path);
        files_reopen();
        return;
    }
    modal_open(path, g_entries[idx].name);
}

static void remote_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_remote_n || g_busy) return;
    g_busy = true;
    static char name[DS_FILE_NAME_MAX];
    snprintf(name, sizeof(name), "%s", g_remote_names[idx]);
    files_thread(op_worker_download, name);
}

/* ---- 顶部按钮 ---- */

static void toggle_view_cb(lv_event_t *e)
{
    (void)e;
    g_remote = !g_remote;
    files_reopen();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    list_start();
}

/* ---- 页面构建 ---- */

static void bottom_btn(lv_obj_t *scr, const char *txt, lv_coord_t x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 140, 42);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -12);
    lv_obj_set_style_radius(b, 21, 0);
    ui_label(b, txt, &lv_font_cn_14, UI_TEXT);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}

void ui_files_build(lv_obj_t *screen, int app_id)
{
    g_app_id = app_id;
    g_list = g_path_lbl = g_msg_lbl = g_modal = NULL;
    if (g_dir[0] == '\0') snprintf(g_dir, sizeof(g_dir), "%s", ds_sd_root());

    ui_gesture_back_add(screen);

    lv_obj_t *title = ui_label(screen, "文件", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    g_path_lbl = ui_label(screen, g_remote ? "Bridge 文件" : g_dir,
                          &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_set_width(g_path_lbl, 400);
    lv_label_set_long_mode(g_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(g_path_lbl, LV_ALIGN_TOP_MID, 0, 34);

    g_list = lv_obj_create(screen);
    lv_obj_set_size(g_list, 440, 330);
    lv_obj_align(g_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_all(g_list, 0, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);

    g_msg_lbl = ui_label(screen, "", &lv_font_cn_12, UI_OK);
    lv_obj_set_width(g_msg_lbl, 380);
    lv_label_set_long_mode(g_msg_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(g_msg_lbl, LV_ALIGN_TOP_MID, 0, 392);
    lv_obj_add_flag(g_msg_lbl, LV_OBJ_FLAG_HIDDEN);

    bottom_btn(screen, g_remote ? "返回 TF" : "Bridge 文件", -78, toggle_view_cb);
    bottom_btn(screen, "刷新", 78, refresh_cb);

    g_busy = false;
    list_start();
}

void ui_files_on_exit(void)
{
    g_gen++; /* 在途 worker 的回调据此丢弃（防悬垂控件） */
    g_busy = false;
    g_list = g_path_lbl = g_msg_lbl = NULL;
    if (g_modal) {
        lv_obj_delete(g_modal);
        g_modal = NULL;
    }
}