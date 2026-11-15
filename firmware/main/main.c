/* AI Gadget 固件（Phase 6 后半：模拟器 UI 移植上真机，ADR-001 共享源）。
 *
 * 启动序（顺序是踩坑结论，勿调整）：
 *   1. I2C 扫描（证据）+ PMU + 探针（RTC/PMU/TF）
 *   2. 音频 codec 初始化（audio_esp32：ES7210 采/ES8311 播，语音链路）
 *   3. 显示 + ds_init 切 bridge（首屏空态而非 mock）+ ui_init()
 *      （模拟器共享 UI）+ 充电屏保挂 lv_layer_top（跨 App 屏常驻）
 *   4. Wi-Fi 最后启动；GOT_IP → SNTP 对时
 *      （UI 构建必须先于 Wi-Fi：NVS 缓存凭据 2.4s 拿 IP 的竞态教训）
 *
 * KEY3（GPIO18）：屏保态=唤醒；正常态=ui_key_dispatch（Web 可配映射）。
 * PWR/BOOT 短按接入与 KEY3 长按（PTT）：待按键输入服务阶段。
 */
#include "bsp/esp-bsp.h"
#include "voice/audio_esp32.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lv_adapter.h"
#include "http_client.h"
#include "hal/gpio_ll.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "power_manager.h"
#include "ui.h"
#include "ui_keys.h"
#include "data_source.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* bring-up Wi-Fi 凭据（gitignore，不进仓库）；缺文件则跳过联网 */
#if __has_include("wifi_secret.h")
#include "wifi_secret.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#define BRIDGE_HOST ""
#endif

static const char *TAG = "gadget";

/* ---- 显示锁：必须用 -1（无限等待） ----
 * bsp_display_lock() 文档写「timeout_ms=0 无限等待」，但实现直接转
 * esp_lv_adapter_lock(timeout_ms)，而 adapter 把 0 当 0 tick（非阻塞
 * try-lock），失败只返回 false。返回值一被忽略，后果就是：main 在
 * LVGL 后台任务渲染中并发改 widget → 命中 LVGL 断言
 * "Invalidate area is not allowed during rendering"，而
 * LV_ASSERT_HANDLER 默认 while(1) → 主任务静默死锁、屏上只剩半帧
 *（上白下绿）、IDLE0 每 30s 报 task_wdt（真机实测 2026-11-15）。
 * 统一经下面两个包装器 + -1 = portMAX_DELAY。 */
static inline void display_lock(void) { (void)esp_lv_adapter_lock(-1); }
static inline void display_unlock(void) { esp_lv_adapter_unlock(); }

/* ---- 开机卡死临时探针（定位完删除）----
 * 每 5s：①以 200ms 试锁 LVGL 判断显示锁是否被永久占用；
 * ②打印任务表（B=阻塞 R=就绪 S=挂起）定位谁在等谁。
 * 依赖 CONFIG_FREERTOS_USE_TRACE_FACILITY + STATS_FORMATTING_FUNCTIONS。 */
/* 一次性连通性自检：GET /api/v1/bots（不需要用户操作就能验证
 * 设备→Bridge 这条 HTTP 路径；语音链路卡 15s 超时时先看这个）。
 * 注意任务栈要 ≥12KB：http_client.c 的 GET/POST 路径有 2~4KB 栈缓冲
 *（曾用 6KB → 栈溢出踩坏堆，表现为 LVGL 任务里 time() 的 newlib 锁
 * 断言 xQueueSemaphoreTake failed: uxItemSize == 0）。 */
static void net_probe_once(void)
{
    /* 与语音请求同一条通道（data/http_client.c 的 BSD socket 实现） */
    static char body[256];
    size_t blen = 0;
    int64_t t0 = esp_timer_get_time();
    int st = http_get_bin(BRIDGE_HOST "/api/v1/bots", body, sizeof(body), &blen,
                          5000);
    ESP_LOGW(TAG, "net probe: status=%d body=%u dur=%lldms", st,
             (unsigned)blen, (long long)((esp_timer_get_time() - t0) / 1000));
}

static void hang_probe_task(void *arg)
{
    (void)arg;
    static char tbuf[1024];
    bool net_done = false;
    for (int i = 1;; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!net_done && i >= 3) {
            net_done = true;
            net_probe_once();
        }
        esp_err_t r = esp_lv_adapter_lock(200);
        ESP_LOGI(TAG, "probe#%d lvgl_lock=%s heap_int=%u", i,
                 r == ESP_OK ? "FREE" : "BUSY",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (r == ESP_OK) {
            esp_lv_adapter_unlock();
        } else {
            /* 只在锁被长期占用时 dump 任务表 */
            vTaskList(tbuf);
            ESP_LOGW(TAG, "tasks:\nName\tState\tPrio\tStack\tNum\n%s", tbuf);
        }
    }
}

/* 事件位 */
#define EV_KEY3_BIT BIT0  /* KEY3 按下沿（去抖后；key_task 判短/长按） */
#define EV_NET_BIT BIT1   /* Wi-Fi 拿到 IP → 对时 */
#define EV_BOOT_BIT BIT2  /* BOOT(GPIO0) 短按 = 返回 */
#define EV_KEY3_REL_BIT BIT3 /* KEY3 释放沿（长按 PTT 收口） */

static EventGroupHandle_t g_ev;

static const struct {
    uint8_t addr;
    const char *chip;
} g_expected[] = {
    { 0x34, "AXP2101 PMU" },
    { 0x51, "PCF85063 RTC" },
    { 0x6B, "QMI8658 IMU" },
    { 0x18, "ES8311 codec" },
    { 0x40, "ES7210 ADC" },
    { 0x5A, "CST9220 touch" },
};

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "i2c bus not available (bsp_i2c_get_handle)");
        return;
    }

    ESP_LOGI(TAG, "=== I2C scan (0x08-0x77) ===");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(bus, addr, 50);
        if (err == ESP_OK) {
            const char *known = "";
            for (size_t i = 0; i < sizeof(g_expected) / sizeof(g_expected[0]); i++) {
                if (g_expected[i].addr == addr) {
                    known = g_expected[i].chip;
                    break;
                }
            }
            ESP_LOGI(TAG, "  0x%02X ACK  %s", addr, known);
        }
    }
    ESP_LOGI(TAG, "=== scan done（对照 docs/HARDWARE.md §2.6）===");
}

/* ---- 探针（bring-up 保留项，结果进日志/HARDWARE.md） ---- */
static esp_err_t i2c_read_u8(uint8_t dev_addr, uint8_t reg, uint8_t *val)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t h;
    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev, &h);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(h, &reg, 1, val, 1, 50);
    i2c_master_bus_rm_device(h);
    return err;
}

static void probe_rtc(void)
{
    uint8_t s = 0, m = 0, h = 0;
    if (i2c_read_u8(0x51, 0x03, &s) == ESP_OK &&
        i2c_read_u8(0x51, 0x04, &m) == ESP_OK &&
        i2c_read_u8(0x51, 0x05, &h) == ESP_OK) {
        bool osc_stop = (s & 0x80) != 0;
        ESP_LOGI(TAG, "rtc: BCD %02x:%02x:%02x%s", h & 0x3f, m & 0x7f, s & 0x7f,
                 osc_stop ? "（OSC 停振，时间无效，首次接电池需校时）" : "");
    } else {
        ESP_LOGW(TAG, "rtc read failed");
    }
}

static void probe_pmu_raw(void)
{
    uint8_t r00, r01, r03;
    bool ok = i2c_read_u8(0x34, 0x00, &r00) == ESP_OK;
    ok = ok && i2c_read_u8(0x34, 0x01, &r01) == ESP_OK;
    ok = ok && i2c_read_u8(0x34, 0x03, &r03) == ESP_OK;
    if (ok) {
        ESP_LOGI(TAG, "axp2101 raw: reg00=0x%02X reg01=0x%02X reg03=0x%02X",
                 r00, r01, r03);
    } else {
        ESP_LOGW(TAG, "axp2101 raw read failed");
    }
}

static void probe_sdcard(void)
{
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK || bsp_sdcard == NULL) {
        ESP_LOGW(TAG, "sdcard mount: %s（未插卡可忽略）", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "sdcard: mounted, cid.name=%s", bsp_sdcard->cid.name);
}

/* ---- 自建显示管线（复刻 bsp_display_start，两处真机调优） ----
 * 1) buffer_height 50→20（480×20×2=19.2KB/块）：Wi-Fi 上线后内部 RAM
 *    最大连续块实测仅 ~31KB，50 行分块（48KB）的 SPI DMA 分配失败
 *    →"Draw bitmap failed: NO_MEM"→ 花屏卡死（屏保全屏渲染首触）；
 * 2) LVGL 任务栈入 PSRAM。max_transfer_sz 同步按块匹配（BSP 原值 450KB）。 */
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static lv_display_t *display_start_custom(void)
{
    esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    lv_cfg.stack_in_psram = true;
    lv_cfg.task_stack_size = 16 * 1024; /* 页面刷新回调里同步跑 HTTP+cJSON */
    ESP_ERROR_CHECK(esp_lv_adapter_init(&lv_cfg));

    const bsp_display_config_t disp_config = {
        .max_transfer_sz = 480 * 20 * 2, /* SPI 分块上限（内部 DMA 预算内） */
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_new(&disp_config, &panel, &io));

    /* 分块渲染：20 行/块（19.2KB，内部 DMA 水位内）。真机教训：
     * buffer_height=480 全帧刷会触发 DMA 洪水→花屏（同 bring-up 坑#2）；
     * QSPI 也不支持 TEAR DOUBLE_FULL。顺滑度靠 PSRAM 120MHz + 后续
     * 渲染优化补。 */
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = 480,
            .ver_res = 480,
            .buffer_height = 20,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (disp == NULL) return NULL;
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    /* 触摸参数与 BSP 默认一致（swap_xy=1 / mirror_y=1） */
    bsp_display_cfg_t touch_sel = {
        .touch_flags = { .swap_xy = 1, .mirror_x = 0, .mirror_y = 1 },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(bsp_touch_new(&touch_sel, &tp));
    const esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    if (esp_lv_adapter_register_touch(&touch_cfg) == NULL) return NULL;

    ESP_ERROR_CHECK(bsp_display_brightness_init());
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    return disp;
}

/* ---- 充电屏保（PLAN §13.4）：挂 lv_layer_top，跨 App 屏常驻 ---- */
static lv_obj_t *g_saver;
static lv_obj_t *g_saver_clock;
static lv_obj_t *g_saver_batt;
static lv_obj_t *g_saver_dot;
static volatile bool g_saver_on;
static int64_t g_key_activity_us; /* KEY3 非触摸，LVGL inactivity 不感知 */
static int g_user_brightness = 100;
#define SAVER_IDLE_MS (2 * 60 * 1000)
#define SAVER_BRIGHTNESS 15

static void saver_enter(void)
{
    if (g_saver_on) return;
    g_saver_on = true;
    lv_obj_clear_flag(g_saver, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(g_saver, -1); /* 顶到 layer_top 最上（状态栏之上） */
    bsp_display_brightness_set(SAVER_BRIGHTNESS);
    ESP_LOGI(TAG, "screensaver ON (brightness %d%%)", SAVER_BRIGHTNESS);
}

static void saver_exit(void)
{
    if (!g_saver_on) return;
    g_saver_on = false;
    lv_obj_add_flag(g_saver, LV_OBJ_FLAG_HIDDEN);
    bsp_display_brightness_set(g_user_brightness);
    ESP_LOGI(TAG, "screensaver OFF (brightness %d%%)", g_user_brightness);
}

static void heap_report(const char *where)
{
    ESP_LOGI(TAG, "heap[%s]: total=%u internal=%u largest_int=%u largest_dma=%u",
             where, (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_free_internal_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void saver_tick(lv_timer_t *t)
{
    (void)t;
    battery_status_t st = power_manager_snapshot();

    static int beat = 0;
    if (++beat >= 30) { /* 30s 心跳：内存水位观测（UI 移植期诊断） */
        beat = 0;
        heap_report("tick");
    }

    /* idle = LVGL 输入不活动时间 与 KEY3 距今 较小者 */
    uint32_t lv_idle = lv_display_get_inactive_time(NULL);
    int64_t key_idle = (esp_timer_get_time() - g_key_activity_us) / 1000;
    int64_t idle_ms = key_idle < (int64_t)lv_idle ? key_idle : (int64_t)lv_idle;

    if (!g_saver_on) {
        if (st.usb_connected && idle_ms >= SAVER_IDLE_MS) {
            saver_enter();
        }
        return;
    }

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    lv_label_set_text(g_saver_clock, buf);

    if (st.charging) {
        lv_label_set_text_fmt(g_saver_batt, "%d%% charging", st.battery_percent);
    } else if (st.usb_connected) {
        lv_label_set_text_fmt(g_saver_batt, "%d%% usb", st.battery_percent);
    } else {
        lv_label_set_text_fmt(g_saver_batt, "%d%%", st.battery_percent);
    }

    /* 呼吸点绕圆缓移 + 透明度呼吸（AMOLED 烧屏防护：无固定亮区） */
    static float angle = 0;
    static int phase = 0;
    angle += 0.7f;
    phase++;
    const int cx = 240, cy = 225, r = 185;
    float rad = angle * (float)M_PI / 180.0f;
    lv_obj_set_pos(g_saver_dot,
                   (int)(cx + r * cosf(rad)) - 6, (int)(cy + r * sinf(rad)) - 6);
    lv_obj_set_style_bg_opa(g_saver_dot,
                            (lv_opa_t)(110 + 90 * sinf(phase * 0.12f)), 0);
}

static void saver_press_cb(lv_event_t *e)
{
    (void)e;
    saver_exit();
}

static void saver_build(void)
{
    g_saver = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_saver, 480, 480);
    lv_obj_set_pos(g_saver, 0, 0);
    lv_obj_set_style_bg_color(g_saver, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_saver, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_saver, 0, 0);
    lv_obj_set_style_radius(g_saver, 0, 0);
    lv_obj_set_style_pad_all(g_saver, 0, 0);
    lv_obj_set_scrollbar_mode(g_saver, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_saver, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_saver, saver_press_cb, LV_EVENT_PRESSED, NULL);

    g_saver_clock = lv_label_create(g_saver);
    lv_label_set_text(g_saver_clock, "--:--");
    lv_obj_set_style_text_font(g_saver_clock, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(g_saver_clock, lv_color_hex(0x8A8F9E), 0);
    lv_obj_align(g_saver_clock, LV_ALIGN_CENTER, 0, -10);

    g_saver_batt = lv_label_create(g_saver);
    lv_label_set_text(g_saver_batt, "");
    lv_obj_set_style_text_font(g_saver_batt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_saver_batt, lv_color_hex(0x5A5F6E), 0);
    lv_obj_align(g_saver_batt, LV_ALIGN_CENTER, 0, 34);

    g_saver_dot = lv_obj_create(g_saver);
    lv_obj_set_size(g_saver_dot, 12, 12);
    lv_obj_set_style_radius(g_saver_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_saver_dot, lv_color_hex(0x7C5CFF), 0);
    lv_obj_set_style_bg_opa(g_saver_dot, LV_OPA_40, 0);
    lv_obj_set_style_border_width(g_saver_dot, 0, 0);
    lv_obj_clear_flag(g_saver_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(g_saver_dot, 0, 0);

    lv_timer_create(saver_tick, 1000, NULL);
    g_key_activity_us = esp_timer_get_time();
}

/* LVGL 存活探针：跑在 LVGL 线程（lv_timer）。若屏幕冻结但日志仍每
 * 15s 出现 → LVGL 活着、是显示/DMA 侧问题；若日志也停 → LVGL 线程
 * 卡在刷新/等待。排障用，长期保留成本可忽略（1 行/15s）。 */
static void lvgl_heartbeat_cb(lv_timer_t *t)
{
    (void)t;
    ESP_LOGI(TAG, "lvgl alive: tick=%lu", (unsigned long)lv_tick_get());
}

/* ---- 实体键：去抖 ISR → 事件位 → 唤醒/UI 分发任务 ---- */
static void IRAM_ATTR key_gpio_isr(void *arg)
{
    /* 机械抖动去抖 200ms（一次按压的多个沿只取第一个；按/放沿共用窗口） */
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 200000) return;
    last_us = now;

    uint32_t bits = (uint32_t)(uintptr_t)arg;
    /* KEY3 配 ANYEDGE：低电平=按下、高电平=释放（释放沿供 PTT 收口）。
     * gpio_ll 内联读寄存器=IRAM 安全（gpio_get_level 在 flash） */
    if (bits & EV_KEY3_BIT) {
        bits = gpio_ll_get_level(&GPIO, 18) == 0 ? EV_KEY3_BIT : EV_KEY3_REL_BIT;
    } else if (bits & EV_BOOT_BIT) {
        if (gpio_ll_get_level(&GPIO, 0) != 0) return; /* BOOT 释放沿忽略 */
    }
    BaseType_t wakeup = pdFALSE;
    xEventGroupSetBitsFromISR(g_ev, bits, &wakeup);
    portYIELD_FROM_ISR(wakeup);
}

/* KEY3 短/长按状态机（key_task 内）：按下沿起算，<600ms 释放=短按；
 * ≥600ms=长按（PTT begin），松开沿=长按释放（PTT end） */
#define KEY3_LONG_MS 600
static int s_key3_state; /* 0=空闲 1=按下未定 2=长按中 */
static int64_t s_key3_down_us;

static void key_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 按下未定时按剩余毫秒等待（到点即判长按），其余无限等 */
        TickType_t wait = portMAX_DELAY;
        if (s_key3_state == 1) {
            int64_t remain_us = KEY3_LONG_MS * 1000LL -
                                (esp_timer_get_time() - s_key3_down_us);
            if (remain_us <= 0) wait = 0;
            else wait = (TickType_t)(remain_us / 1000 / portTICK_PERIOD_MS) + 1;
        }
        EventBits_t bits = xEventGroupWaitBits(
            g_ev, EV_KEY3_BIT | EV_KEY3_REL_BIT | EV_BOOT_BIT,
            pdTRUE, pdFALSE, wait);
        g_key_activity_us = esp_timer_get_time();
        int64_t now = esp_timer_get_time();

        if (bits & EV_KEY3_BIT) {
            s_key3_state = 1;
            s_key3_down_us = now;
            if (g_saver_on) { /* 屏保态按键立即唤醒（不进本次按压分发） */
                display_lock();
                saver_exit();
                display_unlock();
            }
        }
        if (bits & EV_BOOT_BIT) {
            display_lock();
            if (g_saver_on) {
                saver_exit();
            } else {
                ui_key_dispatch(UI_KEY_BOOT); /* 默认=返回（Web 可配） */
            }
            display_unlock();
        }
        if (bits & EV_KEY3_REL_BIT) {
            if (s_key3_state == 1) { /* 短按：松开即分发 */
                s_key3_state = 0;
                display_lock();
                ui_key_dispatch(UI_KEY_KEY3);
                display_unlock();
            } else if (s_key3_state == 2) { /* 长按释放：PTT 收口 */
                s_key3_state = 0;
                display_lock();
                ui_key_dispatch_long_release(UI_KEY_KEY3);
                display_unlock();
            }
        }
        if (s_key3_state == 1 && now - s_key3_down_us >= KEY3_LONG_MS * 1000LL) {
            s_key3_state = 2; /* 长按达成（屏保已由按下沿唤醒） */
            display_lock();
            ui_key_dispatch_long(UI_KEY_KEY3);
            display_unlock();
        }
    }
}

/* GOT_IP 后补拉 Web 配置（ui_remote_config_refresh 会动 LVGL，需持锁） */
static void remote_cfg_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300)); /* 让 sta 连接稳定（DHCP 后立刻发首包偶发失败） */
    display_lock();
    ui_remote_config_refresh();
    display_unlock();
    ESP_LOGI(TAG, "remote config applied (theme/bg/keys)");
    vTaskDelete(NULL);
}

/* ---- Wi-Fi：GOT_IP → SNTP 对时 ---- */
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retry");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "wifi got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(g_ev, EV_NET_BIT);
        /* 联网后才拉得到 Web 下发配置（主题/背景/按键）。必须用独立
         * 大栈任务：http_client 单请求要 4KB 栈缓冲，事件回调栈只有 4KB。 */
        static bool cfg_task_started;
        if (!cfg_task_started) {
            cfg_task_started = true;
            xTaskCreate(remote_cfg_task, "cfg_fetch", 12288, NULL, 4, NULL);
        }
    }
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(g_ev, EV_NET_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        /* SNTP 对时（屏保时钟/会话时间戳） */
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "pool.ntp.org");
        esp_sntp_init();
    }
}

/* TCP/IP 栈 + 事件循环：必须在**任何 socket 调用之前**起来。
 * 真机事故（2026-11-15）：UI 初始化会拉 Bridge 外观/按键配置，
 * 那时栈还没起（原来栈初始化在 wifi_start 里，排在 UI 之后）→
 * lwip assert "tcpip_send_msg_wait_sem: Invalid mbox" → 重启循环。
 * Wi-Fi 关联仍留在 wifi_start（保持"UI 先于 Wi-Fi"的顺序约束）。 */
static void net_stack_init(void)
{
    static bool done;
    if (done) return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    done = true;
}

static void wifi_start(void)
{
    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "wifi_secret.h 缺失，跳过 Wi-Fi");
        return;
    }
    net_stack_init();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL));
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi connecting to \"%s\" ...", WIFI_SSID);
}

void app_main(void)
{
    i2c_scan();

    esp_err_t pm_err = power_manager_init(bsp_i2c_get_handle());
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "power_manager unavailable: %s", esp_err_to_name(pm_err));
    }
    probe_rtc();
    probe_pmu_raw();
    probe_sdcard();

    /* 音频：codec+I2S+定标一步到位（真机语音 ops 的采集/播放句柄） */
    if (audio_esp32_init() != 0) {
        ESP_LOGW(TAG, "audio init failed，语音链路降级");
    }

    g_ev = xEventGroupCreate();
    xTaskCreate(key_task, "key_dispatch", 4096, NULL, 5, NULL);
    xTaskCreate(net_task, "net_switch", 5120, NULL, 5, NULL);

    /* 实体键输入：KEY3(GPIO18) 短按=App 自定义；BOOT(GPIO0) 短按=返回。
     * （PWRON 为 PMU 键，IRQ 未接 ESP32——Muse 坑#2，短按检测待 AXP2101
     * IRQ 状态轮询方案，PLAN §13.3。）长按语义在固件侧：BOOT 长按=下载，
     * PWRON 长按=PMU 关机，均不可配置。 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << 18) | (1ULL << 0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE, /* KEY3 按住 PTT 需释放沿；
                                           BOOT 释放沿被状态机忽略 */
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(18, key_gpio_isr, (void *)(uintptr_t)EV_KEY3_BIT));
    ESP_ERROR_CHECK(gpio_isr_handler_add(0, key_gpio_isr, (void *)(uintptr_t)EV_BOOT_BIT));

    /* 显示 + 共享 UI + 屏保，先于 Wi-Fi（竞态教训）。
     * 数据源在 ui_init 前即切 bridge：首屏读到的是「空」而非 mock
     * 演示数据（无配置 → 空态提示），联网后各页轮询拿真实数据；
     * 同任务切换也消除了原 GOT_IP 后跨任务改 g_impl 的竞态 */
    /* 网络栈先起（UI 初始化会立刻拉 Bridge 配置） */
    net_stack_init();

    heap_report("pre-display");
    if (display_start_custom() == NULL) {
        ESP_LOGE(TAG, "display_start_custom failed");
        return;
    }
    /* 临时：开机卡死探针（定位完删除） */
    xTaskCreate(hang_probe_task, "hangprobe", 12288, NULL, 3, NULL);

    heap_report("post-display");

    if (strlen(BRIDGE_HOST) > 0) {
        ds_init("bridge", BRIDGE_HOST, NULL);
        ESP_LOGI(TAG, "data source -> bridge(%s)", BRIDGE_HOST);
    }

    display_lock();
    ui_init();
    saver_build();
    lv_timer_create(lvgl_heartbeat_cb, 15000, NULL);
    display_unlock();
    heap_report("post-ui");

    /* 背光延后到首帧构建完：真机实测（2026-11-15）在 ui_init 之前调
     * bsp_display_backlight_on() 会卡死在 esp_lcd_panel_io_tx_param 等
     * SPI 总线——此时适配器首帧刷屏的 DMA 传输在途，主任务 tx_param
     * 拿不到总线（LVGL 锁反而是空闲的，说明不是 GUI 线程问题）。
     * 延后既避免死锁，也避免开机闪一屏未初始化的花屏。 */
    bsp_display_backlight_on();

    wifi_start();
    ESP_LOGI(TAG, "gadget up: shared UI + screensaver; wifi last");
}
