/* data_source_devstats — 系统状态采集（设置→系统状态仪表盘）。
 * 单文件双平台（模式同 data_source_files.c）：
 *   真机：heap_caps 水位 / 温度传感器 / 分区表遍历 / FATFS 用量 /
 *         空闲优先级自旋采样估算双核平均负载 / esp_timer 运行时长；
 *   模拟器：设计稿量级演示值 + TF 卡用 sdcard/ 目录真实占用（statvfs）。
 * 全程阻塞（FATFS 扫描/Flash 段遍历），只能 worker 线程调用（坑 #11）。
 */
#include "data_source.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "bsp/esp-bsp.h"
#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h" /* IDF6：esp_ota.h 更名 esp_ota_ops.h */
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "devstats";

/* ---- CPU 负载：空闲优先级自旋采样 ----
 * 原理：不绑核 prio0 任务只在任一核空闲时被调度（同优先级与 idle
 * 按时间片轮转），每 tick 至多跑一次 yield 循环 → 每秒自旋数正比于
 * 双核总空闲容量。负载 = 1 - rate/max_rate（max 自校准取历史峰值的
 * 缓慢衰减值，设备从不会绝对空闲，读数偏低估即可接受——仪表盘用途）。
 * 选自旋而非 FreeRTOS runtime stats：后者要开全局
 * CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS（每次上下文切换都加计时，
 * 影响全局），自旋方案零配置零侵入。静态栈 2KB（坑 #13/#15 纪律，
 * 内部 RAM 静态预算 +~2.4KB，post-ui 70KB 底线仍 >56KB）。 */
static volatile uint32_t s_spin_cnt;
static float s_spin_rate_max; /* 自旋速率峰值（自校准） */
static StackType_t s_spin_stack[2048];
static StaticTask_t s_spin_tcb;
static bool s_spin_started;

static void spin_task(void *arg)
{
    (void)arg;
    for (;;) {
        s_spin_cnt++;
        taskYIELD();
    }
}

static int cpu_load_sample(void)
{
    static uint32_t prev_cnt;
    static int64_t prev_us;
    static bool prev_valid;
    if (!s_spin_started) {
        if (xTaskCreateStatic(spin_task, "spin", sizeof(s_spin_stack),
                              NULL, tskIDLE_PRIORITY, s_spin_stack,
                              &s_spin_tcb) == NULL) {
            return -1; /* 极小概率失败（静态缓冲不会 ENOMEM）*/
        }
        s_spin_started = true;
        return -1; /* 首次只启动，下个窗口出数 */
    }
    uint32_t now = s_spin_cnt;
    int64_t us = esp_timer_get_time();
    if (!prev_valid) {
        prev_cnt = now;
        prev_us = us;
        prev_valid = true;
        return -1;
    }
    float dt = (float)(us - prev_us) / 1e6f;
    if (dt < 1.0f) return -1; /* 窗口太短，不可信 */
    float rate = (float)(uint32_t)(now - prev_cnt) / dt;
    prev_cnt = now;
    prev_us = us;
    /* 峰值跟踪：轻微衰减防永久锁死在历史极端值 */
    s_spin_rate_max *= 0.9995f;
    if (rate > s_spin_rate_max) s_spin_rate_max = rate;
    if (s_spin_rate_max < 1.0f) return -1;
    int load = (int)(100.0f - rate * 100.0f / s_spin_rate_max + 0.5f);
    if (load < 0) load = 0;
    if (load > 100) load = 100;
    return load;
}

/* ---- 芯片温度（惰性安装，S3 快速 RC 型，±1℃ 级精度） ---- */
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_bad;

static float cpu_temp_read(void)
{
    if (s_tsens_bad) return -1.0f;
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 60);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens_bad = true;
            return -1.0f;
        }
    }
    float c = -1.0f;
    temperature_sensor_get_celsius(s_tsens, &c); /* IDF6：read→get 更名 */
    return c;
}

/* ---- app 镜像实际占用：遍历段头（不 hash 全镜像，几十次 8B 读） ----
 * 段头区起点探测：IDF 各版本 esp_image_header_t 长度不同（16/20/24B），
 * 取首个 load_addr 落在 XIP/IRAM/DRAM 映射段的偏移为准，逐段累加
 * 8B 段头 + data_len（段长 4 字节对齐，bootloader esp_image_format.c
 * 校验过），末尾 +32B SHA-256、16B 对齐填充。任一步不合法返回 0
 * （UI 按"未知"显示总量）。 */
static uint32_t app_image_used(const esp_partition_t *part)
{
    uint8_t hdr[2];
    if (esp_partition_read(part, 0, hdr, 2) != ESP_OK || hdr[0] != 0xE9) return 0;
    int segs = hdr[1];
    if (segs <= 0 || segs > 64) return 0;
    uint32_t seg0 = 0;
    for (uint32_t off = 16; off <= 32; off += 4) {
        uint32_t la;
        if (esp_partition_read(part, off, &la, 4) != ESP_OK) return 0;
        uint8_t top = (uint8_t)(la >> 24);
        if (top == 0x3C || top == 0x3F || top == 0x40 || top == 0x42 ||
            top == 0x60) { /* DROM/DRAM/IRAM/IROM/RTC */
            seg0 = off;
            break;
        }
    }
    if (seg0 == 0) return 0;
    uint32_t pos = seg0;
    for (int i = 0; i < segs; i++) {
        uint32_t sh[2];
        if (esp_partition_read(part, pos, sh, 8) != ESP_OK) return 0;
        uint32_t data_len = sh[1];
        if (data_len == 0 || (data_len & 3) || data_len > (32u << 20)) return 0;
        pos += 8 + data_len;
    }
    pos += 32;                  /* 末尾 SHA-256 */
    pos = (pos + 15) & ~15u;    /* 16B 对齐填充 */
    return pos;
}

bool ds_devstats_fetch(ds_devstats_t *out)
{
    memset(out, 0, sizeof(*out));

    /* 内存水位（min 是开机以来最低水位，正是"历史最低"） */
    out->sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->sram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* Flash：程序分区（占用走段头遍历）+ storage 内置 FATFS 分区 */
    const esp_partition_t *app = esp_ota_get_running_partition();
    if (app) {
        out->flash_app_total = app->size;
        out->flash_app_used = app_image_used(app);
        if (out->flash_app_used == 0 || out->flash_app_used > app->size) {
            out->flash_app_used = 0; /* 探测失败按未知处理 */
        }
    }
    const esp_partition_t *sto =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (sto) out->flash_fs_total = sto->size;
    /* storage 分区当前固件不挂载（TF 优先，ADR-010）——容量如实展示 */

    /* TF 卡：BSP 挂载句柄在即为已插卡，FATFS 用量走 VFS 信息接口 */
    out->tf_mounted = (bsp_sdcard != NULL);
    if (out->tf_mounted) {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info("/sdcard", &total, &freeb) == ESP_OK && total > 0) {
            out->tf_total = total;
            out->tf_used = (freeb <= total) ? (total - freeb) : 0;
        }
    }

    out->cpu_temp = cpu_temp_read();
    out->cpu_load = cpu_load_sample();

    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const char *model = "ESP32";
    if (ci.model == CHIP_ESP32S3) model = "ESP32-S3";
    else if (ci.model == CHIP_ESP32C3) model = "ESP32-C3";
    snprintf(out->chip, sizeof(out->chip), "%s %d核 %dMHz", model,
             (int)ci.cores, (int)(esp_clk_cpu_freq() / 1000000));
    const esp_app_desc_t *desc = esp_app_get_description(); /* IDF6 更名 */
    snprintf(out->fw_ver, sizeof(out->fw_ver), "v%s", desc->version);

    out->valid = true;
    ESP_LOGI(TAG, "sram %u/%u min %u psram %u/%u app %u/%u tf %llu/%llu "
                  "temp %.1f load %d up %us",
             (unsigned)out->sram_free, (unsigned)out->sram_total,
             (unsigned)out->sram_min,
             (unsigned)out->psram_free, (unsigned)out->psram_total,
             (unsigned)out->flash_app_used, (unsigned)out->flash_app_total,
             (unsigned long long)out->tf_used, (unsigned long long)out->tf_total,
             (double)out->cpu_temp, out->cpu_load, (unsigned)out->uptime_s);
    return true;
}

#else /* ---------------- 模拟器：演示值 + TF 真实占用 ---------------- */

#include <sys/statvfs.h>
#include <stdlib.h>
#include <time.h>

#include "sd_paths.h"

bool ds_devstats_fetch(ds_devstats_t *out)
{
    static time_t t0;
    if (!t0) t0 = time(NULL);
    time_t now = time(NULL);
    int r1 = rand() % 7 - 3; /* ±3 抖动，演示页有"活着"的感觉 */

    /* 设计稿量级（真机值见固件侧） */
    out->sram_total = 400 * 1024;
    out->sram_free = 229 * 1024 - r1 * 1024;
    out->sram_min = 180 * 1024;
    out->psram_total = 8u * 1024 * 1024;
    out->psram_free = (uint32_t)(0.72 * out->psram_total) + r1 * 64 * 1024;
    out->flash_app_total = (uint32_t)(11.94 * 1024 * 1024);
    out->flash_app_used = (uint32_t)(5.79 * 1024 * 1024);
    out->flash_fs_total = 448 * 1024;
    out->flash_fs_mounted = false; /* 真机当前不挂载 storage 分区（TF 优先） */

    /* TF：模拟器 = GADGET_SD_DIR 真实目录占用 */
    struct statvfs vfs;
    if (statvfs(sd_root(), &vfs) == 0 && vfs.f_blocks > 0) {
        out->tf_mounted = true;
        out->tf_total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t freeb = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        out->tf_used = out->tf_total > freeb ? out->tf_total - freeb : 0;
    }

    out->cpu_temp = 42.5f + (float)(rand() % 10) / 10.0f;
    out->cpu_load = 12 + rand() % 8;

    out->uptime_s = (uint32_t)(now - t0);
    snprintf(out->chip, sizeof(out->chip), "ESP32-S3 双核 240MHz");
    snprintf(out->fw_ver, sizeof(out->fw_ver), "v1.0.0-sim");

    out->valid = true;
    return true;
}

#endif /* ESP_PLATFORM */
