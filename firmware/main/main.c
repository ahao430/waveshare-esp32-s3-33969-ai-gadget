/* AI Gadget 固件（Phase 6 bring-up 第 2 批：触摸/音频/亮度/旋转）。
 * 上电流程：
 *   1. I2C 全地址扫描（证据见 docs/HARDWARE.md §2.7）
 *   2. 电源管理（AXP2101 轮询，PMU 缺失不阻塞）
 *   3. LVGL 测试面板：
 *      - 触摸：按下出现圆形指示器 + 坐标读数（§2.2 单点验证）
 *      - 亮度滑条：拖动调 0-100%（§2.1）
 *      - TONE 键 / KEY3(GPIO18)：440Hz 正弦 2s（§2.3 播放）
 *      - REC 键：录音 2s，屏显 peak/RMS（§2.3 录音非零校验）
 *      - ROT 键：0/90/180/270 旋转循环（§2.1）
 * 音频走独立 FreeRTOS 任务（esp_codec_dev_write 阻塞调用，不占 LVGL 线程）。
 */
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "power_manager.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gadget";

/* ---- 音频参数（BSP 默认 I2S：mono duplex 16bit 22050Hz） ---- */
#define AUD_RATE 22050
#define TONE_HZ 440
#define TONE_SECONDS 2
#define TONE_SAMPLES (AUD_RATE * TONE_SECONDS)
#define REC_SECONDS 2
#define REC_SAMPLES (AUD_RATE * REC_SECONDS)

/* 音频任务事件位 */
#define EV_TONE_BIT BIT0
#define EV_REC_BIT BIT1

static EventGroupHandle_t g_audio_ev;
static esp_codec_dev_handle_t g_spk;
static esp_codec_dev_handle_t g_mic;
static int16_t *g_aud_buf; /* PSRAM：tone 播放 / 录音共用 */

/* ---- LVGL 控件引用 ---- */
static lv_obj_t *g_lbl_status;
static lv_obj_t *g_touch_dot;
static int g_touch_count;
static char g_status_buf[128];

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

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_buf, sizeof(g_status_buf), fmt, ap);
    va_end(ap);
    bsp_display_lock(0);
    lv_label_set_text(g_lbl_status, g_status_buf);
    lv_obj_center(g_lbl_status);
    bsp_display_unlock();
    ESP_LOGI(TAG, "status: %s", g_status_buf);
}

/* ---- 音频任务：TONE / REC ---- */
static void audio_task(void *arg)
{
    (void)arg;
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUD_RATE,
    };

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(g_audio_ev, EV_TONE_BIT | EV_REC_BIT,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EV_TONE_BIT) {
            if (g_spk == NULL) {
                set_status("spk init failed");
                continue;
            }
            /* 生成 440Hz 正弦（幅值 0.35，防爆音） */
            for (int i = 0; i < TONE_SAMPLES; i++) {
                g_aud_buf[i] = (int16_t)(0.35 * 32767 *
                                         sinf(2 * (float)M_PI * TONE_HZ * i / AUD_RATE));
            }
            esp_codec_dev_set_out_vol(g_spk, 70);
            int t0 = (int)esp_timer_get_time();
            esp_err_t err = esp_codec_dev_open(g_spk, &fs) == 0 ? ESP_OK : ESP_FAIL;
            if (err == ESP_OK) {
                err = esp_codec_dev_write(g_spk, g_aud_buf, TONE_SAMPLES * 2) == 0
                          ? ESP_OK
                          : ESP_FAIL;
                esp_codec_dev_close(g_spk);
            }
            int ms = ((int)esp_timer_get_time() - t0) / 1000;
            set_status("TONE 440Hz done\n%d ms %s", ms,
                       err == ESP_OK ? "(audible?)" : "(write FAILED)");
        }
        if (bits & EV_REC_BIT) {
            if (g_mic == NULL) {
                set_status("mic init failed");
                continue;
            }
            memset(g_aud_buf, 0, REC_SAMPLES * 2);
            esp_codec_dev_set_in_gain(g_mic, 24.0); /* 42dB 削波（peak 顶 32767），降档 */
            esp_err_t err = esp_codec_dev_open(g_mic, &fs) == 0 ? ESP_OK : ESP_FAIL;
            if (err == ESP_OK) {
                err = esp_codec_dev_read(g_mic, g_aud_buf, REC_SAMPLES * 2) == 0
                          ? ESP_OK
                          : ESP_FAIL;
                esp_codec_dev_close(g_mic);
            }
            if (err != ESP_OK) {
                set_status("REC read FAILED");
                continue;
            }
            int64_t sum_sq = 0; /* int32 在 peak≈32767 时溢出（实测 2147483647） */
            int16_t peak = 0;
            for (int i = 0; i < REC_SAMPLES; i++) {
                int16_t s = g_aud_buf[i];
                sum_sq += (int64_t)s * s;
                if (s < 0) s = -s;
                if (s > peak) peak = s;
            }
            int rms = (int)sqrtf((float)(sum_sq / REC_SAMPLES));
            set_status("REC 2s\npeak=%d rms=%d %s", peak, rms,
                       peak > 1000 ? "(speaks!)" : "(quiet?)");
        }
    }
}

/* ---- KEY3（GPIO18，低有效）→ TONE ---- */
static void IRAM_ATTR key3_isr(void *arg)
{
    BaseType_t wakeup = pdFALSE;
    xEventGroupSetBitsFromISR(g_audio_ev, EV_TONE_BIT, &wakeup);
    portYIELD_FROM_ISR(wakeup);
}

static void btn_tone_cb(lv_event_t *e)
{
    (void)e;
    xEventGroupSetBits(g_audio_ev, EV_TONE_BIT);
}

static void btn_rec_cb(lv_event_t *e)
{
    (void)e;
    xEventGroupSetBits(g_audio_ev, EV_REC_BIT);
}

static void slider_brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    bsp_display_brightness_set(v);
    ESP_LOGI(TAG, "brightness -> %d%%", v);
}

/* 屏幕级触摸事件：按下/拖动显示指示器与坐标，松手隐藏（§2.2） */
static void screen_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        if (code == LV_EVENT_PRESSED) g_touch_count++;
        /* 回调在 LVGL 线程内，直接操作控件（无需再加显示锁） */
        lv_obj_set_pos(g_touch_dot, p.x - 18, p.y - 18);
        lv_obj_set_hidden(g_touch_dot, false);
        lv_obj_t *lbl = lv_obj_get_child(g_touch_dot, 0);
        lv_label_set_text_fmt(lbl, "%d,%d", (int)p.x, (int)p.y);
        ESP_LOGD(TAG, "touch %s (%d,%d)",
                 code == LV_EVENT_PRESSED ? "down" : "move", p.x, p.y);
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_set_hidden(g_touch_dot, true);
        ESP_LOGI(TAG, "touch released (total %d)", g_touch_count);
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 118, 56);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

/* ---- §2.4 TF 卡：挂载 + 32B 写读校验（无卡只告警） ---- */
static void probe_sdcard(void)
{
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK || bsp_sdcard == NULL) {
        ESP_LOGW(TAG, "sdcard mount: %s（未插卡可忽略）", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "sdcard: mounted, cid.name=%s", bsp_sdcard->cid.name);

    const char *path = BSP_SD_MOUNT_POINT "/bringup.txt";
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "sdcard write: open failed");
        return;
    }
    fputs("bringup-ok-20261114", f);
    fclose(f);

    char buf[32] = { 0 };
    f = fopen(path, "r");
    if (f != NULL) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    ESP_LOGI(TAG, "sdcard r/w: %s",
             strcmp(buf, "bringup-ok-20261114") == 0 ? "OK" : "MISMATCH");
}

/* ---- 共享 I2C 单字节读探针 ---- */
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

/* ---- §2.6 PCF85063 RTC：BCD 读时分秒（sec.7=OSC 停振标志） ---- */
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

/* ---- §2.5 AXP2101 原始寄存器（对表解释待电池接入后做） ---- */
static void probe_pmu_raw(void)
{
    uint8_t r00, r01, r03;
    bool ok = i2c_read_u8(0x34, 0x00, &r00) == ESP_OK;
    ok = ok && i2c_read_u8(0x34, 0x01, &r01) == ESP_OK;
    ok = ok && i2c_read_u8(0x34, 0x03, &r03) == ESP_OK;
    if (ok) {
        ESP_LOGI(TAG, "axp2101 raw: reg00=0x%02X reg01=0x%02X reg03=0x%02X（datasheet 对表待做）",
                 r00, r01, r03);
    } else {
        ESP_LOGW(TAG, "axp2101 raw read failed");
    }
}

static void build_ui(void)
{
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B1020), 0);

    /* 标题（用户反馈“字小”→ 32 号） */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "AI Gadget");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEAF0FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *sub = lv_label_create(root);
    lv_label_set_text(sub, "bringup 2: touch / audio / brightness");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7F8CBB), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 68);

    /* 状态区（中央） */
    g_lbl_status = lv_label_create(root);
    lv_label_set_text(g_lbl_status, "ready");
    lv_obj_set_style_text_font(g_lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_lbl_status, lv_color_hex(0x9FE870), 0);
    lv_obj_center(g_lbl_status);
    lv_obj_set_y(g_lbl_status, -30);

    /* 亮度滑条 */
    lv_obj_t *slider = lv_slider_create(root);
    lv_obj_set_width(slider, 300);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 100, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -130);

    /* 按钮行 */
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 400, 60);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *b1 = make_btn(row, "TONE", btn_tone_cb);
    lv_obj_t *b2 = make_btn(row, "REC", btn_rec_cb);
    (void)b1;
    (void)b2;

    /* 触摸指示器（默认隐藏） */
    g_touch_dot = lv_obj_create(root);
    lv_obj_set_size(g_touch_dot, 36, 36);
    lv_obj_set_style_radius(g_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_touch_dot, lv_color_hex(0x7C5CFF), 0);
    lv_obj_set_style_bg_opa(g_touch_dot, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_touch_dot, 0, 0);
    lv_obj_set_hidden(g_touch_dot, true);
    lv_obj_set_clickable(g_touch_dot, false);
    lv_obj_set_style_pad_all(g_touch_dot, 0, 0);
    lv_obj_t *dot_lbl = lv_label_create(g_touch_dot);
    lv_label_set_text(dot_lbl, "");
    lv_obj_set_style_text_font(dot_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dot_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(dot_lbl);

    /* 屏幕级触摸事件（ bubbled from indev） */
    lv_obj_add_event_cb(root, screen_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, screen_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, screen_touch_cb, LV_EVENT_RELEASED, NULL);
}

void app_main(void)
{
    i2c_scan();

    esp_err_t pm_err = power_manager_init(bsp_i2c_get_handle());
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "power_manager unavailable: %s（UI 显示 USB/-- 兜底）",
                 esp_err_to_name(pm_err));
    }
    probe_rtc();
    probe_pmu_raw();
    probe_sdcard();

    /* 音频：I2S + codec（speaker 的 pa_pin=46 由 BSP 管理） */
    ESP_ERROR_CHECK(bsp_audio_init(NULL)); /* 默认 mono duplex 16bit 22050Hz */
    g_spk = bsp_audio_codec_speaker_init();
    g_mic = bsp_audio_codec_microphone_init();
    ESP_LOGI(TAG, "codec: spk=%p mic=%p", (void *)g_spk, (void *)g_mic);

    g_aud_buf = heap_caps_malloc(TONE_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    if (g_aud_buf == NULL) {
        /* PSRAM 分配失败兜底内部 RAM（tone 与 rec 取大者） */
        g_aud_buf = heap_caps_malloc(TONE_SAMPLES * 2, MALLOC_CAP_INTERNAL);
    }
    assert(g_aud_buf != NULL);

    g_audio_ev = xEventGroupCreate();
    xTaskCreate(audio_task, "audio_test", 4096, NULL, 5, NULL);

    /* KEY3（GPIO18）：外部上拉 3V3，按下接地 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 18,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(18, key3_isr, NULL));

    bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);
    build_ui();
    bsp_display_unlock();

    ESP_LOGI(TAG, "bringup3 ready: 触摸/滑条/TONE/REC；KEY3=TONE");
}
