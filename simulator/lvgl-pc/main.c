/* gadget_sim 入口：SDL 窗口 480×480（对应 33969 AMOLED 分辨率）。
 * 显示/输入走 LVGL 内建 SDL 驱动；业务 UI 在 src/ui/（与固件共享）。
 *
 * 演示/验收钩子（环境变量，与 data_source 钩子同一套约定）：
 *   GADGET_DEMO_APP=<id>   启动即进入指定 App（ui_init 内处理）
 *   GADGET_SNAP=<ppm>[,ms] 延时 ms（默认 2500）把当前屏幕离屏快照为 PPM
 *                          后退出——headless（SDL_VIDEODRIVER=dummy）下做
 *                          确定性 UI 验收，不受真实鼠标/窗口遮挡干扰。
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui.h"

/* 离屏快照到调用方提供的缓冲（绕开 LVGL 内存池：屏幕+图层两张
 * 921KB 快照超出 1MB 池，池内二次分配必失败——lv_snapshot_take
 * 返回 NULL 的根因）。缓冲为 480×480×4，栈放不下用 static。 */
static uint8_t g_snap_scr_buf[480 * 480 * 4];
static uint8_t g_snap_top_buf[480 * 480 * 4];

static lv_draw_buf_t *snap_take(lv_obj_t *obj, lv_color_format_t cf,
                                uint8_t *mem)
{
    lv_draw_buf_t *db = malloc(sizeof(*db));
    if (!db) return NULL;
    lv_draw_buf_init(db, 480, 480, cf, LV_STRIDE_AUTO, mem, 480 * 480 * 4);
    if (lv_snapshot_take_to_draw_buf(obj, cf, db) != LV_RESULT_OK) {
        free(db);
        return NULL;
    }
    return db;
}

static void snap_buf_free(lv_draw_buf_t *db)
{
    /* 缓冲是 static 包装，只释放包裹结构本身（lv_draw_buf_destroy 会
     * 尝试 free 数据指针，不可用于外部缓冲） */
    free(db);
}

static void snap_write(const char *path)
{
    /* 屏幕本体 + lv_layer_top（全局状态栏 overlay 挂在这里）分别快照，
     * 再按 alpha 合成——lv_snapshot 只渲染传入对象子树，不含图层。 */
    lv_draw_buf_t *scr = snap_take(lv_screen_active(),
                                   LV_COLOR_FORMAT_XRGB8888, g_snap_scr_buf);
    lv_draw_buf_t *top = snap_take(lv_layer_top(),
                                   LV_COLOR_FORMAT_ARGB8888, g_snap_top_buf);
    if (!scr) {
        fprintf(stderr, "GADGET_SNAP: lv_snapshot_take failed\n");
        snap_buf_free(top);
        return;
    }

    uint32_t w = scr->header.w, h = scr->header.h;
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *srow = scr->data + y * scr->header.stride;
            const uint8_t *trow = top ? top->data + y * top->header.stride : NULL;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *sp = srow + x * 4; /* B,G,R,X */
                int r = sp[2], g = sp[1], b = sp[0];
                if (trow) {
                    const uint8_t *tp = trow + x * 4; /* B,G,R,A */
                    int a = tp[3];
                    if (a > 0) {
                        r = (tp[2] * a + r * (255 - a)) / 255;
                        g = (tp[1] * a + g * (255 - a)) / 255;
                        b = (tp[0] * a + b * (255 - a)) / 255;
                    }
                }
                uint8_t rgb[3] = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        fprintf(stderr, "GADGET_SNAP: %s (%ux%u)\n", path, w, h);
    }
    snap_buf_free(scr);
    snap_buf_free(top);
}

static void snap_timer_cb(lv_timer_t *t)
{
    snap_write((const char *)lv_timer_get_user_data(t));
    exit(0);
}

int main(void)
{
    lv_init();

    /* SDL 毫秒时钟直接作为 LVGL tick 源 */
    lv_tick_set_cb(SDL_GetTicks);

    lv_display_t *disp = lv_sdl_window_create(480, 480);
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_t *keyboard = lv_sdl_keyboard_create(); /* textarea 键盘输入 */
    (void)disp;
    (void)mouse;
    (void)keyboard;

    ui_init();

    const char *snap = getenv("GADGET_SNAP");
    if (snap && snap[0]) {
        char path[256];
        int ms = 2500;
        const char *comma = strchr(snap, ',');
        if (comma) {
            ms = atoi(comma + 1);
            snprintf(path, sizeof(path), "%.*s", (int)(comma - snap), snap);
        } else {
            snprintf(path, sizeof(path), "%s", snap);
        }
        lv_timer_create(snap_timer_cb, (uint32_t)ms, strdup(path));
    }

    for (;;) {
        lv_timer_handler();
        usleep(3000); /* ~300fps 上限，降低空闲 CPU */
    }
    return 0;
}
