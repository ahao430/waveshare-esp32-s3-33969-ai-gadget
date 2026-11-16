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
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "power_manager.h"
#include "wifi_prov.h"
#include "ui.h"
#include "ui_keys.h"
#include "data_source.h"
#include "voice_session.h"

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

/* 编译期预置的 Bridge 地址簿（逗号分隔，wifi_secret.h 可选）：
 * BRIDGE_HOST 决定"没配置时用哪个"，SEED 只是把这些地址并进设备的
 * 地址簿（幂等、不改当前地址），切换仍在设备 UI 完成——公网服务器
 * 地址很长，设备软键盘输入体验差，预置后开机即出现在簿里。 */
#ifndef BRIDGE_SEED_HOSTS
#define BRIDGE_SEED_HOSTS ""
#endif

/* 可选：开机把"当前 Bridge"强制切到这个地址（部署新服务器后免设备点按）。
 * 空=不启用；它会覆盖设备上手动选择，用完记得删。 */
#ifndef BRIDGE_ACTIVATE_HOST
#define BRIDGE_ACTIVATE_HOST ""
#endif

/* 可选：一次性把"当前 Wi-Fi"切到该 SSID（换网/部署时免设备软键盘输入）。
 * 旧网络仍留在多网络列表（自动连接兜底）；同样用完删。 */
#ifndef WIFI_ACTIVATE_SSID
#define WIFI_ACTIVATE_SSID ""
#endif
#ifndef WIFI_ACTIVATE_PASS
#define WIFI_ACTIVATE_PASS ""
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
/* 连通性自检：GET <bridge>/api/v1/bots（不需要用户操作就能验证设备→
 * Bridge 这条 HTTP 路径；语音链路卡 15s 超时时先看这个）。 */
static void net_probe_once(void)
{
    /* 与语音请求同一条通道（data/http_client.c 的 BSD socket 实现）。
     * Bridge 地址运行期可配（ADR-009）：NVS 优先，编译期宏兜底 */
    char url[192];
    char host[160];
    if (!ds_bridge_host_get(host, sizeof(host))) {
        ESP_LOGW(TAG, "net probe: bridge host 未配置，跳过");
        return;
    }
    snprintf(url, sizeof(url), "%s/api/v1/bots", host);
    static char body[256];
    size_t blen = 0;
    extern int http_connect_stage;
    extern int http_connect_errno;
    int64_t t0 = esp_timer_get_time();
    int st = http_get_bin(url, body, sizeof(body), &blen, 5000);
    /* stage：0=ok 1=fcntl 2=connect 立即失败 3=select 超时 4=SO_ERROR
     *（公网/访客网到达性问题看这里：3=被网络挡住/不可达） */
    ESP_LOGW(TAG, "net probe: status=%d body=%u dur=%lldms stage=%d errno=%d url=%s",
             st, (unsigned)blen, (long long)((esp_timer_get_time() - t0) / 1000),
             http_connect_stage, http_connect_errno, host);
}

/* 一次性自检（默认关）：开机 8s 进 AstrBot 页、11s 弹选择面板，
 * 供无触摸环境下验证面板链路；验证完保持 0。 */
#ifndef GADGET_SELFTEST_BOTPICK
#define GADGET_SELFTEST_BOTPICK 0
#endif
#ifndef GADGET_SELFTEST_VOICE
#define GADGET_SELFTEST_VOICE 0
#endif
/* 开机 12s 自动扫一次打印结果（验证扫描链路用），平时保持 0 */
#ifndef GADGET_SELFTEST_SCAN
#define GADGET_SELFTEST_SCAN 0
#endif
#if GADGET_SELFTEST_SCAN
static void selftest_scan_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(12000)); /* 等自动连上已保存网络 */
    /* 验证真实路径：连接态下调 wifi_prov_scan（新逻辑应不再断开、直接出列表） */
    static wifi_prov_ap_t aps[24];
    int n = 0;
    int rc = wifi_prov_scan(aps, 24, &n);
    ESP_LOGW(TAG, "SELFTEST scan rc=%d n=%d", rc, n);
    for (int i = 0; i < n && i < 8; i++) {
        ESP_LOGW(TAG, "SELFTEST ap[%d] %s rssi=%d", i, aps[i].ssid,
                 (int)aps[i].rssi);
    }
    vTaskDelete(NULL);
}
#endif
#if GADGET_SELFTEST_BOTPICK || GADGET_SELFTEST_VOICE
static void selftest_botpick_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    display_lock();
    ui_open_app(UI_APP_CHAT);
    display_unlock();
    vTaskDelay(pdMS_TO_TICKS(3000));
#if GADGET_SELFTEST_BOTPICK
    ui_conversation_debug_bot_picker();
#endif
#if GADGET_SELFTEST_VOICE
    /* 语音回归：走完整 PTT 状态机（LISTENING→THINKING→SPEAKING），
     * 真实采集麦克风 1.5s 后提交——复现用户"说话→思考→回答"全路径 */
    vs_ptt_begin();
    vTaskDelay(pdMS_TO_TICKS(1500));
    vs_ptt_end();
    for (int i = 0; i < 40; i++) { /* 观察 20s 状态机 */
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGW(TAG, "SELFTEST vs state=%d heap_int=%u", (int)vs_state(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#endif
    vTaskDelete(NULL);
}
#endif

/* LVGL 心跳（定义在 saver_tick；此处前置声明供看门狗使用） */
static volatile uint32_t g_lvgl_beat;

/* LVGL 探针 + 自愈看门狗。
 * 背景：启动期显示管线偶发卡死（约 30-50% 复位概率，HARDWARE §2.10：
 * lvgl 任务持锁卡在 flush/渲染内部），此前只能人工断电。这里连续
 * 3 次（15s）拿不到锁 → 判定挂死，记录后 esp_restart 自愈——相当于
 * 用户"再断电重开一次"的自动化。运行期误触发风险：正常 UI 不会连续
 * 15s 持锁；真触发也只损失一次重启。 */
/* net_probe 独立任务：HTTP connect 可能阻塞数秒（Bridge 不可达时），
 * 不能占着看门狗任务——曾把 TWDT 喂狗拖超时导致健康启动也被重启
 * （2026-11-16 真机定位）。 */
static void net_probe_task(void *arg)
{
    (void)arg;
    net_probe_once();
    vTaskDelete(NULL);
}

static void hang_probe_task(void *arg)
{
    (void)arg;
    static char tbuf[1024];
    bool net_done = false;
    int busy_streak = 0;
    uint32_t last_beat = 0;
    /* 注册任务看门狗：若本任务自己卡死（实测：楔死时它卡在
     * esp_lv_adapter_lock 里不再打印），TWDT 30s 后 panic 重启。 */
    esp_task_wdt_add(NULL);
    for (int i = 1;; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!net_done && i >= 3) {
            net_done = true;
            xTaskCreate(net_probe_task, "net_probe", 8192, NULL, 3, NULL);
        }
        /* 非阻塞 try-lock：带超时的锁在楔死时会把探针自己挂住
         * （200ms 那次实测连 probe#3 都出不来），try-lock 才能持续探测 */
        esp_err_t r = esp_lv_adapter_lock(0);
        ESP_LOGI(TAG, "probe#%d lvgl_lock=%s heap_int=%u beat=%u", i,
                 r == ESP_OK ? "FREE" : "BUSY",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)g_lvgl_beat);
        if (r != ESP_OK) {
            /* 冻屏定位（2026-11-16 根因：ds_state 惰性刷新在 LVGL 回调里
             * 同步发 HTTP）：锁被占时打印 lvgl 任务行（B=阻塞 R=就绪）。 */
            vTaskList(tbuf);
            const char *p = strstr(tbuf, "lvgl");
            if (p) {
                char line[96] = "";
                sscanf(p, "%95[^\n]", line);
                ESP_LOGW(TAG, "probe#%d lvgl task: %s", i, line);
            }
        }
        if (r == ESP_OK) esp_lv_adapter_unlock();

        /* 判据是心跳是否推进（不是锁是否 BUSY）：LVGL 定时器 1s 一次，
         * 连续 15s 没推进才算真楔死 → 重启自愈。 */
        if (g_lvgl_beat == last_beat) {
            busy_streak++;
            if (busy_streak >= 3) {
                vTaskList(tbuf);
                ESP_LOGE(TAG, "LVGL heartbeat stuck %ds — watchdog restart (HARDWARE §2.10)",
                         busy_streak * 5);
                ESP_LOGW(TAG, "tasks:\nName\tState\tPrio\tStack\tNum\n%s", tbuf);
                esp_restart();
            }
        } else {
            last_beat = g_lvgl_beat;
            busy_streak = 0;
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
    ds_files_ensure_dirs(); /* backup/cache/appearance/downloads（ADR-010） */
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
        .max_transfer_sz = 480 * 10 * 2, /* SPI 分块上限（内部 DMA 预算内） */
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
            .buffer_height = 10,
            /* 绘制缓冲放内部 RAM（DMA 直连，SPI 发送零拷贝）。放 PSRAM
             * 时每块都要在内部 RAM 临时分配 9.6~19.2KB bounce buffer，
             * 聊天页打开后内存被切碎 → 分配失败 → 花屏 + flush 卡死
             * （2026-11-15 真机反复复现）。10 行双缓冲 = 19.2KB，
             * 开机预分配，运行期不再申请大块。语音线程同步改为静态栈
             * 常驻任务（ui_conversation.c voice_start）。 */
            .use_psram = false,
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
#define SAVER_IDLE_MS (2 * 60 * 1000)
#define SAVER_BRIGHTNESS 15

/* 用户亮度 = ds_state()->brightness（状态栏快捷面板/设置页滑条在改）；
 * 屏保/关机恢复读它，不再写死 100。 */
static int user_brightness(void)
{
    int b = ds_state()->brightness;
    return (b >= 10 && b <= 100) ? b : 100;
}

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
    bsp_display_brightness_set(user_brightness());
    ESP_LOGI(TAG, "screensaver OFF (brightness %d%%)", user_brightness());
}

static void heap_report(const char *where)
{
    ESP_LOGI(TAG, "heap[%s]: total=%u internal=%u largest_int=%u largest_dma=%u",
             where, (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_free_internal_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

/* LVGL 心跳：本回调跑在 LVGL 任务里。看门狗用它判断"真卡死"还是
 * "采样瞬间恰好在重绘"——只看锁 BUSY 会误杀（2026-11-15 实测：
 * 低内存下重绘变慢，三次采样都撞上 BUSY，但 lvgl alive tick 仍在推进）。 */
static void saver_tick(lv_timer_t *t)
{
    (void)t;
    g_lvgl_beat++;
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

/* PWR 键长按 → 优雅关机：灭屏给反馈 → 软件关机（REG10 bit0）。
 * PMU 10s 硬关兜底（battery_keys_init 配置）；软关后 USB 在位时
 * PMU 可能立即重新上电（VBUS 触发开机）——属硬件行为，拔线即可。 */
static void pwr_graceful_shutdown(void)
{
    ESP_LOGW(TAG, "PWR long-press: graceful shutdown");
    display_lock();
    bsp_display_brightness_set(0);
    display_unlock();
    vTaskDelay(pdMS_TO_TICKS(250)); /* 让灭屏指令走完 QSPI */

    esp_err_t err = power_manager_power_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "software power-off failed: %s (10s hard-off fallback)",
                 esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    /* 还在跑（软关失败/USB 在位被重新上电）：恢复亮度继续运行 */
    ESP_LOGW(TAG, "still alive after power-off cmd (USB attached?) - resume");
    display_lock();
    bsp_display_brightness_set(user_brightness());
    display_unlock();
}

#define PWR_POLL_MS 100
static void key_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 按下未定时按剩余毫秒等待（到点即判长按）；其余最多等
         * PWR_POLL_MS——PWR 键走 PMU 状态寄存器轮询，没有 GPIO 中断 */
        TickType_t wait = pdMS_TO_TICKS(PWR_POLL_MS);
        if (s_key3_state == 1) {
            int64_t remain_us = KEY3_LONG_MS * 1000LL -
                                (esp_timer_get_time() - s_key3_down_us);
            if (remain_us <= 0) wait = 0;
            else {
                TickType_t w = (TickType_t)(remain_us / 1000 / portTICK_PERIOD_MS) + 1;
                if (w < wait) wait = w;
            }
        }
        EventBits_t bits = xEventGroupWaitBits(
            g_ev, EV_KEY3_BIT | EV_KEY3_REL_BIT | EV_BOOT_BIT,
            pdTRUE, pdFALSE, wait);
        g_key_activity_us = esp_timer_get_time();
        int64_t now = esp_timer_get_time();

        /* PWR 键（AXP2101 PWRON，状态位轮询；长按=优雅关机） */
        unsigned pwr = power_manager_poll_key();
        if (pwr & BATT_KEY_LONG) {
            pwr_graceful_shutdown();
            continue;
        }
        if (pwr & BATT_KEY_CLICK) {
            g_key_activity_us = esp_timer_get_time(); /* 活动记账 */
            ESP_LOGI(TAG, "PWR short press (no-op for now)");
        }

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

/* GOT_IP 后补拉 Web 配置。prefetch 只跑网络（HTTP 最坏数秒，不持锁，
 * 避免联网初期冻屏）；refresh 动 LVGL（改主题/重建桌面），需持锁 */
static void remote_cfg_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300)); /* 让 sta 连接稳定（DHCP 后立刻发首包偶发失败） */
    ui_remote_config_prefetch();
    display_lock();
    ui_remote_config_refresh();
    display_unlock();
    ESP_LOGI(TAG, "remote config applied (theme/bg/keys/launcher)");
    vTaskDelete(NULL);
}

/* ---- Wi-Fi：GOT_IP → SNTP 对时 ---- */
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 有凭据才连（未配网时 start 仅供扫描；凭据来自 NVS 或宏兜底，
         * wifi_start 已 set_config，这里读回判断） */
        wifi_config_t cur;
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
            cur.sta.ssid[0] != '\0') {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "wifi started, 未配网（设置→网络与连接→Wi-Fi 设置）");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_prov_busy()) return; /* 配网试连期间避让，由 wifi_prov 驱动 */
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

/* 把 BRIDGE_SEED_HOSTS（逗号分隔）逐条并进设备地址簿；幂等，
 * 只追加不激活（当前地址由设备 UI/上次选择决定）。需在 NVS 初始化后调用。 */
static void seed_bridge_hosts(void)
{
    char buf[sizeof(BRIDGE_SEED_HOSTS)] = "";
    strlcpy(buf, BRIDGE_SEED_HOSTS, sizeof(buf));
    char *p = buf;
    while (p && *p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
            *--end = '\0';
        }
        if (*p) {
            wifi_prov_bridge_seed(p);
            ESP_LOGI(TAG, "bridge addr seeded: %s", p);
        }
        if (!comma) break;
        p = comma + 1;
    }

    /* 一次性激活（BRIDGE_ACTIVATE_HOST，非空才生效）：把当前地址切过去。
     * 仅当与当前不同才写 NVS（幂等不磨损 flash）；这是部署新 Bridge 后
     * 免设备点按的过渡开关，用完请从 wifi_secret.h 删除，否则每次开机
     * 都会覆盖设备上手动选择的地址。 */
    if (BRIDGE_ACTIVATE_HOST[0] != '\0') {
        char cur[160] = "";
        if (!wifi_prov_bridge_get(cur, sizeof(cur)) ||
            strcmp(cur, BRIDGE_ACTIVATE_HOST) != 0) {
            wifi_prov_bridge_save(BRIDGE_ACTIVATE_HOST);
            ESP_LOGW(TAG, "bridge host activated: %s", BRIDGE_ACTIVATE_HOST);
        }
    }

    /* 配网 TF 恢复（ADR-010）：NVS 无凭据（刷机/擦除）且 TF 有
     * backup/provision.json → 自动恢复，无需重配。 */
    {
        char s0[33], p0[WIFI_PROV_PASS_MAX];
        if (!wifi_prov_load(s0, sizeof(s0), p0, sizeof(p0))) {
            (void)wifi_prov_restore_from_backup();
        } else {
            wifi_prov_backup_now(); /* 确保 TF 上有最新备份（幂等重写） */
        }
    }

    /* 一次性 Wi-Fi 激活：把当前网络切到 WIFI_ACTIVATE_SSID（旧网络仍在
     * 多网络列表，开机磋商兜底）。wifi_start 在 net_stack_init 之后跑，
     * 同一次开机即直连新网络。 */
    if (WIFI_ACTIVATE_SSID[0] != '\0') {
        char cur_ssid[33], cur_pass[WIFI_PROV_PASS_MAX];
        if (!wifi_prov_load(cur_ssid, sizeof(cur_ssid), cur_pass, sizeof(cur_pass)) ||
            strcmp(cur_ssid, WIFI_ACTIVATE_SSID) != 0) {
            wifi_prov_save(WIFI_ACTIVATE_SSID, WIFI_ACTIVATE_PASS);
            ESP_LOGW(TAG, "wifi activated: %s", WIFI_ACTIVATE_SSID);
        }
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
    /* 地址簿预置要在 ds_init 读地址之前完成 */
    seed_bridge_hosts();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    done = true;
}

/* 多网络磋商任务：阻塞扫描 + 试连，独立 6KB 栈（http 无关） */
static void prov_autoconnect_task(void *arg)
{
    (void)arg;
    int rc = wifi_prov_autoconnect_others();
    if (rc == 1) {
        ESP_LOGI(TAG, "auto network switch: connected to saved network");
    } else if (rc < 0) {
        ESP_LOGW(TAG, "auto network switch: scan failed");
    }
    vTaskDelete(NULL);
}

static void wifi_start(void)
{
    net_stack_init();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* 凭据优先级：NVS（配网页保存，ADR-009）→ wifi_secret.h 编译兜底 */
    char ssid[33] = "", pass[WIFI_PROV_PASS_MAX] = "";
    const char *src = "wifi_secret.h";
    if (wifi_prov_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        src = "nvs";
    } else if (strlen(WIFI_SSID) > 0) {
        strlcpy(ssid, WIFI_SSID, sizeof(ssid));
        strlcpy(pass, WIFI_PASS, sizeof(pass));
    } else {
        ssid[0] = '\0'; /* 未配网：仍 start（扫描/配网需要），不 connect */
    }

    if (ssid[0] != '\0') {
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (ssid[0] != '\0') {
        ESP_LOGI(TAG, "wifi connecting to \"%s\" (creds=%s) ...", ssid, src);
        if (strcmp(src, "wifi_secret.h") == 0) {
            /* 宏兜底首次生效即落 NVS（并进入多网络列表）：宏从此只是
             * 出厂值，后续以设备内配置为准 */
            wifi_prov_save(WIFI_SSID, WIFI_PASS);
        }
    } else {
        ESP_LOGW(TAG, "wifi 未配网，等待设置页配置（扫描可用）");
    }

    /* 多网络自动连接（家庭/公司多环境）：已保存 ≥2 个网络时，开机
     * 3s 内没连上当前网络就在列表里找一个在场的切过去 */
    if (wifi_prov_has_multi()) {
        xTaskCreate(prov_autoconnect_task, "prov_auto", 6144, NULL, 3, NULL);
    }
}

void app_main(void)
{
    i2c_scan();

    esp_err_t pm_err = power_manager_init(bsp_i2c_get_handle());
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "power_manager unavailable: %s", esp_err_to_name(pm_err));
    } else {
        /* PWR 键：长按优雅关机 + 10s 硬关兜底（2026-11-16） */
        esp_err_t key_err = power_manager_keys_init();
        if (key_err != ESP_OK) {
            ESP_LOGW(TAG, "PWR key arm failed: %s", esp_err_to_name(key_err));
        }
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
    /* 8KB：4KB 实测栈溢出（vTaskList + 日志），12KB→8KB 省 4KB 内部
     * RAM 给 SPI DMA bounce buffer */
    xTaskCreate(hang_probe_task, "hangprobe", 8192, NULL, 3, NULL);
#if GADGET_SELFTEST_BOTPICK || GADGET_SELFTEST_VOICE
    xTaskCreate(selftest_botpick_task, "selftest", 12288, NULL, 3, NULL);
#endif
#if GADGET_SELFTEST_SCAN
    xTaskCreate(selftest_scan_task, "st_scan", 6144, NULL, 3, NULL);
#endif

    heap_report("post-display");

    /* 数据源切 bridge：地址运行期可配（NVS 优先，ADR-009），配网页
     * 改地址后无需重刷（下次 ds_init/HTTP 即生效） */
    char bridge_host[160];
    if (ds_bridge_host_get(bridge_host, sizeof(bridge_host))) {
        ds_init("bridge", bridge_host, NULL);
        ESP_LOGI(TAG, "data source -> bridge(%s)", bridge_host);
    } else {
        ESP_LOGW(TAG, "bridge host 未配置（设置→网络与连接 可配）");
    }

    display_lock();
    ui_init();
    saver_build();
    lv_timer_create(lvgl_heartbeat_cb, 15000, NULL);
    display_unlock();
    heap_report("post-ui");

    /* 背光延后到首帧构建完 + 持显示锁串行化：真机实测（2026-11-15）
     * 在 ui_init 之前调 bsp_display_backlight_on() 会卡死在
     * esp_lcd_panel_io_tx_param 等 SPI 总线——适配器首帧刷屏 DMA 在途。
     * 仅延后还不够：2026-11-16 实证延后后仍撞车——主任务无锁发 polling
     * 事务，与 LVGL 任务的 QSPI 传输并发，撑 125s 后
     * spi_device_release_bus 断言崩溃（连续对话首测触发）。持显示锁
     * 与适配器互斥后，正常启动窗口安全；楔死启动则阻塞至 TWDT 自愈
     * （不再走到断言路径）。 */
    display_lock();
    bsp_display_backlight_on();
    display_unlock();

    wifi_start();
    ESP_LOGI(TAG, "gadget up: shared UI + screensaver; wifi last");
}
