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

static void snap_write(const char *path)
{
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(),
                                          LV_COLOR_FORMAT_XRGB8888);
    if (!buf) {
        fprintf(stderr, "GADGET_SNAP: lv_snapshot_take failed\n");
        return;
    }

    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", buf->header.w, buf->header.h);
        for (uint32_t y = 0; y < buf->header.h; y++) {
            const uint8_t *row = buf->data + y * buf->header.stride;
            for (uint32_t x = 0; x < buf->header.w; x++) {
                const uint8_t *p = row + x * 4; /* XRGB8888 小端：B,G,R,X */
                uint8_t rgb[3] = { p[2], p[1], p[0] };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        fprintf(stderr, "GADGET_SNAP: %s (%ux%u)\n", path,
                buf->header.w, buf->header.h);
    }
    lv_draw_buf_destroy(buf);
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
