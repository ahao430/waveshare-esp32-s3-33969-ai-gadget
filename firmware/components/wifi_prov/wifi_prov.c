/* wifi_prov.c — 运行期配网实现：NVS 存取 + 阻塞扫描 + 试连。
 * 事件等待用独立事件组（GOT_IP / 断开两位），与 main.c 的 wifi_evt
 * 并存——配网期间 wifi_evt 靠 wifi_prov_busy() 避让自动重连。 */
#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "cJSON.h"

static const char *TAG = "wifi_prov";

#define WIFI_PROV_NS "wifi"
#define BIT_GOT_IP   (1 << 0)
#define BIT_DROP     (1 << 1) /* 等待期间被断开（密码错/被踢） */

static EventGroupHandle_t s_ev;
static volatile bool s_busy;
static volatile int s_drop_reason; /* 最近一次断开 reason（等待循环判密码错用） */

/* TF 自动备份（定义在文件末尾；save/remove 里调用） */
static void prov_backup_write(void);

/* ---------------- NVS ---------------- */

static bool nvs_read_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_PROV_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

static void nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_PROV_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (val && val[0] != '\0') {
        (void)nvs_set_str(h, key, val);
    } else {
        (void)nvs_erase_key(h, key);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

bool wifi_prov_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    if (!nvs_read_str("ssid", ssid, ssid_cap)) {
        ssid[0] = '\0';
        pass[0] = '\0';
        return false;
    }
    if (!nvs_read_str("pass", pass, pass_cap)) pass[0] = '\0';
    return true;
}

/* ---------------- 多网络列表（自动切换） ----------------
 * NVS 键 "wl"：每两条一组 "\n" 分隔（ssid/pass）。Wi-Fi 密码不含换行。 */
#define WL_MAX 6
#define WL_BUF 1024

static int wl_load(char ssids[][33], char passes[][WIFI_PROV_PASS_MAX], int max)
{
    char buf[WL_BUF] = "";
    if (!nvs_read_str("wl", buf, sizeof(buf))) return 0;
    int n = 0;
    char *p = buf;
    while (*p && n < max) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        strlcpy(ssids[n], p, 33);
        p = nl + 1;
        nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        strlcpy(passes[n], p, WIFI_PROV_PASS_MAX);
        p = nl + 1;
        n++;
    }
    return n;
}

static void wl_store(char ssids[][33], char passes[][WIFI_PROV_PASS_MAX], int n)
{
    char buf[WL_BUF];
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(buf + off, sizeof(buf) - off, "%s\n%s\n",
                         ssids[i], passes[i]);
        if (w < 0 || (size_t)w >= sizeof(buf) - off) break;
        off += (size_t)w;
    }
    nvs_write_str("wl", buf);
}

void wifi_prov_save(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return;
    nvs_write_str("ssid", ssid); /* 当前网络（下次开机快速路径） */
    nvs_write_str("pass", pass ? pass : "");

    char ssids[WL_MAX][33];
    char passes[WL_MAX][WIFI_PROV_PASS_MAX];
    int n = wl_load(ssids, passes, WL_MAX);
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(ssids[i], ssid) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (n < WL_MAX) {
            idx = n++;
        } else {
            idx = WL_MAX - 1; /* 满了：替换最后一条（最近使用优先保留） */
        }
    }
    strlcpy(ssids[idx], ssid, 33);
    strlcpy(passes[idx], pass ? pass : "", WIFI_PROV_PASS_MAX);
    wl_store(ssids, passes, n);
    prov_backup_write(); /* TF 自动备份（未插卡静默跳过） */
}

bool wifi_prov_has_multi(void)
{
    char ssids[WL_MAX][33];
    char passes[WL_MAX][WIFI_PROV_PASS_MAX];
    return wl_load(ssids, passes, WL_MAX) >= 2;
}

bool wifi_prov_bridge_get(char *out, size_t cap)
{
    return nvs_read_str("bh", out, cap);
}

/* 地址簿最多 8 条、每条 ≤159 字节（NVS 单 string 上限内）。 */
#define BH_MAX 8
#define BH_LEN 160

/* 读地址簿原始串（"\n" 分隔；空串=无） */
static size_t bh_read(char *buf, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_PROV_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, "bh_list", buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { buf[0] = '\0'; return 0; }
    return strlen(buf);
}

static void bh_write(const char *list)
{
    nvs_write_str("bh_list", list);
}

/* 地址簿 upsert：已在簿中不重复追加（不改当前激活地址）。 */
static void bh_upsert(const char *url)
{
    char list[BH_MAX * (BH_LEN + 1) + 1] = "";
    size_t n = bh_read(list, sizeof(list));
    if (strstr(list, url) != NULL) {
        /* 已存在（strstr 足够：URL 互不为子串的概率极高，误判最多多发一条
         * 重复项，切一次即修复） */
        return;
    }
    int lines = 0;
    for (size_t i = 0; i < n; i++) {
        if (list[i] == '\n') lines++;
    }
    if (n > 0) lines++;
    if (lines >= BH_MAX) return; /* 簿满：只激活不追加 */
    size_t off = n;
    if (off > 0 && list[off - 1] != '\n' && off < sizeof(list) - 1) {
        list[off++] = '\n';
    }
    strlcpy(list + off, url, sizeof(list) - off);
    bh_write(list);
}

void wifi_prov_bridge_save(const char *url)
{
    if (!url || url[0] == '\0') return;
    nvs_write_str("bh", url);
    bh_upsert(url);
    prov_backup_write();
}

void wifi_prov_bridge_seed(const char *url)
{
    if (!url || url[0] == '\0') return;
    char cur[BH_LEN] = "";
    if (!wifi_prov_bridge_get(cur, sizeof(cur))) {
        nvs_write_str("bh", url); /* 从未配置过：预置项即默认 */
    }
    bh_upsert(url);
}

int wifi_prov_bridge_list(char *out, size_t cap)
{
    char list[BH_MAX * (BH_LEN + 1) + 1] = "";
    bh_read(list, sizeof(list));
    if (list[0] != '\0') {
        strlcpy(out, list, cap);
        return 0;
    }
    /* 簿空：编译期兜底作为唯一一条（main.c 通过 ds 层给的宏值无法
     * 反查，这里留空由调用方补） */
    out[0] = '\0';
    return -1;
}

int wifi_prov_bridge_remove(const char *url)
{
    char list[BH_MAX * (BH_LEN + 1) + 1] = "";
    size_t n = bh_read(list, sizeof(list));
    if (n == 0 || !url) return -1;
    char cur[BH_LEN] = "";
    if (wifi_prov_bridge_get(cur, sizeof(cur)) && strcmp(cur, url) == 0) {
        return -1; /* 当前激活地址不可删 */
    }
    char out[BH_MAX * (BH_LEN + 1) + 1] = "";
    size_t off = 0;
    bool removed = false;
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && !(len == strlen(url) && strncmp(p, url, len) == 0)) {
            if (off + len + 2 < sizeof(out)) {
                memcpy(out + off, p, len);
                off += len;
                out[off++] = '\n';
            }
        } else if (len > 0) {
            removed = true;
        }
        if (!e) break;
        p = e + 1;
    }
    if (off > 0 && out[off - 1] == '\n') out[off - 1] = '\0';
    bh_write(out);
    if (removed) prov_backup_write();
    return removed ? 0 : -1;
}

/* ---------------- 扫描 ---------------- */

int wifi_prov_scan(wifi_prov_ap_t *out, int max, int *n)
{
    wifi_scan_config_t cfg = { .show_hidden = false };

    /* 扫描路径（2026-11-16 真机对比实验定位"列表永远空"）：
     * - 连接态直接扫：不打断连接，实测 46~63 个 AP；
     * - "正在连接"时 esp_wifi_scan_start 返回 ESP_ERR_WIFI_STATE（旧坑）：
     *   先断开中止，等 DISCONNECTED 事件落定（BIT_DROP，800ms 兜底）再扫，
     *   扫完恢复自动重连。曾用固定 150ms——从已连接态 deauth 要 300ms+，
     *   150ms 后的扫描被驱动静默吞掉（返回 OK 但 0 结果）。 */
    esp_err_t err = esp_wifi_scan_start(&cfg, true); /* block ~1.5-3s */
    if (err == ESP_ERR_WIFI_STATE) {
        char ssid[33], pass[WIFI_PROV_PASS_MAX];
        bool had_creds = wifi_prov_load(ssid, sizeof(ssid), pass, sizeof(pass));
        s_busy = true;             /* wifi_evt 的自动重连在此窗口内避让 */
        esp_wifi_disconnect();     /* 中止进行中的连接尝试 */
        if (s_ev) {
            xEventGroupClearBits(s_ev, BIT_DROP);
            xEventGroupWaitBits(s_ev, BIT_DROP, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(800));
        } else {
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        err = esp_wifi_scan_start(&cfg, true);
        s_busy = false;
        if (had_creds) esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return err == ESP_ERR_WIFI_STATE ? -1 : -2;
    }

    uint16_t total = 0;
    if (esp_wifi_scan_get_ap_num(&total) != ESP_OK || total == 0) {
        *n = 0;
        return 0;
    }
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * total);
    if (!recs) return -2;
    uint16_t got = total;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        free(recs);
        return -2;
    }
    /* 记录按 RSSI 降序；同名 SSID 去重（取最强那个） */
    int cnt = 0;
    for (int i = 0; i < got && cnt < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < cnt; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strlcpy(out[cnt].ssid, ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = recs[i].rssi;
        out[cnt].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out[cnt].enterprise =
            (recs[i].authmode >= WIFI_AUTH_WPA_ENTERPRISE &&
             recs[i].authmode <= WIFI_AUTH_WPA3_ENT_192) ||
            recs[i].authmode == WIFI_AUTH_WAPI_PSK;
        cnt++;
    }
    free(recs);
    *n = cnt;
    ESP_LOGI(TAG, "scan done: raw=%u cnt=%d", (unsigned)total, cnt);
    return 0;
}

/* ---------------- 连接 ---------------- */

static void prov_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (!s_ev) return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_drop_reason = d ? d->reason : 0;
        xEventGroupSetBits(s_ev, BIT_DROP);
    }
}

static void prov_events_ensure(void)
{
    static bool done;
    if (done) return;
    done = true;
    s_ev = xEventGroupCreate();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, prov_evt, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, prov_evt, NULL);
}

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

/* 断开原因是否属"认证/密码类"（快速失败；其余原因继续重试到超时） */
static bool reason_is_auth_fail(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return true;
    default:
        return false;
    }
}

int wifi_prov_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return -1;
    prov_events_ensure();

    char old_ssid[33] = "", old_pass[WIFI_PROV_PASS_MAX] = "";
    bool had_old = wifi_prov_load(old_ssid, sizeof(old_ssid), old_pass, sizeof(old_pass));

    s_busy = true;
    s_drop_reason = 0;
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_DROP);

    /* 先断旧连接；给断开事件 ~100ms 到达窗口后清位——否则旧连接的
     * DISCONNECTED 会被当成新连接失败（真机在公司网踩过：秒失败） */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(s_ev, BIT_DROP);
    s_drop_reason = 0;

    apply_sta_config(ssid, pass);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect(%s) call failed: %s", ssid, esp_err_to_name(err));
        s_busy = false;
        return -1;
    }
    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);

    /* 等待 GOT_IP（15s 上限）：断开事件按 reason 分流——
     * 密码/认证类立即失败；NO_AP/关联类重试（部分 AP 首次关联会抖）。 */
    int rc = -1;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (xTaskGetTickCount() < deadline) {
        EventBits_t bits = xEventGroupWaitBits(
            s_ev, BIT_GOT_IP | BIT_DROP, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        if (bits & BIT_GOT_IP) {
            rc = 0;
            break;
        }
        if (bits & BIT_DROP) {
            int reason = s_drop_reason;
            ESP_LOGW(TAG, "disconnected during prov, reason=%d", reason);
            if (reason_is_auth_fail(reason)) {
                ESP_LOGW(TAG, "auth failure (wrong password or unsupported auth)");
                break;
            }
            /* 其他原因：继续尝试（直到 deadline） */
            vTaskDelay(pdMS_TO_TICKS(500));
            (void)esp_wifi_connect();
        }
    }
    if (rc == 0) {
        wifi_prov_save(ssid, pass); /* 连上才落 NVS */
        ESP_LOGI(TAG, "connected to \"%s\"", ssid);
    } else {
        /* 失败：恢复旧凭据重连（若有），设备回到可配网/可用的已知态 */
        ESP_LOGW(TAG, "provision failed for \"%s\"", ssid);
        esp_wifi_disconnect();
        if (had_old) {
            apply_sta_config(old_ssid, old_pass);
            esp_wifi_connect();
        }
    }
    s_busy = false;
    return rc;
}

int wifi_prov_autoconnect_others(void)
{
    /* 等 3s：同环境开机时当前网络很快连上，不必扫描打断 */
    wifi_ap_record_t ap;
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return 0;

    char ssids[WL_MAX][33];
    char passes[WL_MAX][WIFI_PROV_PASS_MAX];
    int n = wl_load(ssids, passes, WL_MAX);
    if (n < 2) return 0;

    wifi_prov_ap_t aps[24];
    int cnt = 0;
    if (wifi_prov_scan(aps, 24, &cnt) != 0) return -1;
    /* scan 已按信号降序：按序找已保存且在场的网络，逐个试连 */
    for (int i = 0; i < cnt; i++) {
        for (int j = 0; j < n; j++) {
            if (strcmp(aps[i].ssid, ssids[j]) != 0) continue;
            ESP_LOGI(TAG, "auto switch to saved network \"%s\"", ssids[j]);
            if (wifi_prov_connect(ssids[j], passes[j]) == 0) return 1;
        }
    }
    return 0;
}

bool wifi_prov_busy(void) { return s_busy; }

bool wifi_prov_status(char *ssid, size_t ssid_cap, char *ip, size_t ip_cap)
{
    if (ssid_cap) ssid[0] = '\0';
    if (ip_cap) ip[0] = '\0';
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ssid_cap) strlcpy(ssid, (const char *)ap.ssid, ssid_cap);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && ip_cap) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, ip_cap, IPSTR, IP2STR(&info.ip));
        }
    }
    return true;
}

/* ---------------- TF 自动备份 / 恢复（ADR-010，2026-11-16） ----------------
 * 每次配网/地址簿变更后把 NVS 配网数据写 /sdcard/backup/provision.json；
 * 开机 NVS 无凭据但备份存在 → 自动恢复（刷机/擦 NVS 免重配；解决过
 * "擦 NVS 丢家里 Wi-Fi"的痛点）。备份含 Wi-Fi 明文密码——TF 卡属于
 * 设备本体，勿外借（README/DEPLOY 已注明）。
 * 未插卡/写失败全部静默：TF 是增强项，不是依赖项。 */
#define PROV_BACKUP_DIR  "/sdcard/backup"
#define PROV_BACKUP_PATH PROV_BACKUP_DIR "/provision.json"

static void prov_backup_write(void)
{
    char ssid[33] = "", pass[WIFI_PROV_PASS_MAX] = "";
    bool has_wifi = wifi_prov_load(ssid, sizeof(ssid), pass, sizeof(pass));
    char bh[BH_LEN] = "";
    bool has_bh = wifi_prov_bridge_get(bh, sizeof(bh));
    char list[BH_MAX * (BH_LEN + 1) + 1] = "";
    (void)bh_read(list, sizeof(list));
    char wl[WL_BUF] = "";
    (void)nvs_read_str("wl", wl, sizeof(wl));
    if (!has_wifi && !has_bh && list[0] == '\0') return; /* 无可备份 */

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "v", 1);
    if (has_wifi) {
        cJSON_AddStringToObject(root, "ssid", ssid);
        cJSON_AddStringToObject(root, "pass", pass);
    }
    if (has_bh) cJSON_AddStringToObject(root, "bh", bh);
    if (list[0]) cJSON_AddStringToObject(root, "bh_list", list);
    if (wl[0]) cJSON_AddStringToObject(root, "wl", wl);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    (void)mkdir(PROV_BACKUP_DIR, 0777); /* 已存在返回 -1，忽略 */
    FILE *f = fopen(PROV_BACKUP_PATH, "wb");
    if (f) {
        size_t wr = fwrite(json, 1, strlen(json), f);
        fclose(f);
        if (wr == strlen(json)) ESP_LOGI(TAG, "provision backed up to TF");
    }
    free(json);
}

void wifi_prov_backup_now(void) { prov_backup_write(); }

bool wifi_prov_restore_from_backup(void)
{
    FILE *f = fopen(PROV_BACKUP_PATH, "rb");
    if (!f) return false;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    bool restored = false;
    const cJSON *j = cJSON_GetObjectItem(root, "ssid");
    if (cJSON_IsString(j) && j->valuestring[0]) {
        const cJSON *p = cJSON_GetObjectItem(root, "pass");
        /* 直接写 NVS（不调 wifi_prov_save，避免再次触发备份写入） */
        nvs_write_str("ssid", j->valuestring);
        nvs_write_str("pass", cJSON_IsString(p) ? p->valuestring : "");
        restored = true;
    }
    j = cJSON_GetObjectItem(root, "wl");
    if (cJSON_IsString(j)) {
        nvs_write_str("wl", j->valuestring);
    }
    j = cJSON_GetObjectItem(root, "bh");
    if (cJSON_IsString(j)) {
        nvs_write_str("bh", j->valuestring);
        restored = true;
    }
    j = cJSON_GetObjectItem(root, "bh_list");
    if (cJSON_IsString(j)) {
        bh_write(j->valuestring);
    }
    cJSON_Delete(root);
    if (restored) ESP_LOGW(TAG, "provision restored from TF backup");
    return restored;
}
