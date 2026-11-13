/* gadget_sim 入口：SDL 窗口 480×480（对应 33969 AMOLED 分辨率）。
 * 显示/输入走 LVGL 内建 SDL 驱动；业务 UI 在 src/ui/（与固件共享）。
 */
#include <SDL2/SDL.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui.h"

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

    for (;;) {
        lv_timer_handler();
        usleep(3000); /* ~300fps 上限，降低空闲 CPU */
    }
    return 0;
}
