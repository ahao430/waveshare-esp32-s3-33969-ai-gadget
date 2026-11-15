/* wifi_prov.c — 运行期配网实现：NVS 存取 + 阻塞扫描 + 试连。
 * 事件等待用独立事件组（GOT_IP / 断开两位），与 main.c 的 wifi_evt
 * 并存——配网期间 wifi_evt 靠 wifi_prov_busy() 避让自动重连。 */
#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_PROV_NS "wifi"
#define BIT_GOT_IP   (1 << 0)
#define BIT_DROP     (1 << 1) /* 等待期间被断开（密码错/被踢） */

static EventGroupHandle_t s_ev;
static volatile bool s_busy;

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

void wifi_prov_save(const char *ssid, const char *pass)
{
    nvs_write_str("ssid", ssid);
    nvs_write_str("pass", pass);
}

bool wifi_prov_bridge_get(char *out, size_t cap)
{
    return nvs_read_str("bh", out, cap);
}

void wifi_prov_bridge_save(const char *url)
{
    nvs_write_str("bh", url);
}

/* ---------------- 扫描 ---------------- */

int wifi_prov_scan(wifi_prov_ap_t *out, int max, int *n)
{
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&cfg, true); /* block ~1.5-3s */
    if (err == ESP_ERR_WIFI_STATE) return -1;        /* 正在连接，稍后再试 */
    if (err != ESP_OK) return -2;

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
        cnt++;
    }
    free(recs);
    *n = cnt;
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

int wifi_prov_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return -1;
    prov_events_ensure();

    char old_ssid[33] = "", old_pass[WIFI_PROV_PASS_MAX] = "";
    bool had_old = wifi_prov_load(old_ssid, sizeof(old_ssid), old_pass, sizeof(old_pass));

    s_busy = true;
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_DROP);

    /* 已连接时先断（复用同一 STA）；断开事件由 prov_evt 记录，无需等 */
    esp_wifi_disconnect();
    apply_sta_config(ssid, pass);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_busy = false;
        return -1;
    }

    /* 等 GOT_IP；期间被断开（密码错）继续等一小会儿——部分路由器
     * 会先断一次再 DHCP，GOT_IP 到了就当成功 */
    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_DROP,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    int rc = -1;
    if (bits & BIT_GOT_IP) {
        rc = 0;
        wifi_prov_save(ssid, pass); /* 连上才落 NVS */
    } else {
        /* 失败：恢复旧凭据重连（若有），设备回到可配网/可用的已知态 */
        esp_wifi_disconnect();
        if (had_old) {
            apply_sta_config(old_ssid, old_pass);
            esp_wifi_connect();
        }
    }
    s_busy = false;
    return rc;
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
