/* data_source_wifi — 运行期配网（设备本地能力，ADR-009），单文件双平台：
 *   固件（ESP_PLATFORM）= NVS + esp_wifi 真实现（wifi_prov 组件）
 *   模拟器            = 演示数据（固定 AP 列表；密码 "wrong"=失败），
 *                       GADGET_DEMO_WIFI=scan_empty|connect_fail 场景注入
 * 不走 ds_impl_t ops：Wi-Fi 属于设备本地能力（同 battery 的平台分界），
 * 模拟器 bridge 模式同样走演示数据（配网 UI 开发不依赖真硬件）。 */
#include "data_source.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
/* ================= 固件：真实现（转发 wifi_prov 组件） ================= */

#include "wifi_prov.h"

int ds_wifi_scan(ds_wifi_ap_t *out, int max, int *n)
{
    /* 结构布局与 wifi_prov_ap_t 一致（ssid/rssi/secure），直接转发 */
    return wifi_prov_scan((wifi_prov_ap_t *)out, max, n);
}

int ds_wifi_connect(const char *ssid, const char *pass)
{
    return wifi_prov_connect(ssid, pass ? pass : "") == 0 ? 0 : -1;
}

bool ds_wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
    return wifi_prov_status(ssid, (size_t)ssid_cap, ip, (size_t)ip_cap);
}

/* 编译期兜底（与 main.c 的 wifi_secret.h 块同模式；文件在 main/ 下） */
#if __has_include("wifi_secret.h")
#include "wifi_secret.h"
#else
#define BRIDGE_HOST ""
#endif

bool ds_bridge_host_get(char *out, int cap)
{
    if (wifi_prov_bridge_get(out, (size_t)cap)) return true;
    if (BRIDGE_HOST[0] == '\0') {
        out[0] = '\0';
        return false;
    }
    snprintf(out, (size_t)cap, "%s", BRIDGE_HOST);
    return true;
}

void ds_bridge_host_save(const char *url)
{
    wifi_prov_bridge_save(url);
}

int ds_bridge_host_list(char *out, int cap)
{
    char list[1024];
    if (wifi_prov_bridge_list(list, sizeof(list)) == 0 && list[0]) {
        snprintf(out, (size_t)cap, "%s", list);
        return 0;
    }
    /* 簿空/无 NVS：当前生效地址（含编译期兜底）作为唯一一条 */
    if (ds_bridge_host_get(out, cap)) return 0;
    out[0] = '\0';
    return -1;
}

int ds_bridge_host_remove(const char *url)
{
    return wifi_prov_bridge_remove(url);
}

#else
/* ================= 模拟器：演示数据 ================= */

#include <stdlib.h>
#include <time.h>

static char s_saved_host[160]; /* 演示态保存的 Bridge 地址（=当前激活） */
static char s_host_book[4][160]; /* 演示地址簿 */
static int s_host_book_n;
static char s_cur_ssid[33] = "Home-WiFi-5G"; /* 演示态：默认"已连接" */
static bool s_connected = true;

int ds_wifi_scan(ds_wifi_ap_t *out, int max, int *n)
{
    const char *env = getenv("GADGET_DEMO_WIFI");
    if (env && strcmp(env, "scan_empty") == 0) {
        *n = 0;
        return 0;
    }
    static const struct { const char *ssid; int rssi; bool secure; bool ent; } demo[] = {
        { "Home-WiFi-5G", -42, true, false },
        { "Home-WiFi-2.4G", -55, true, false },
        { "Xiaomi_路由器", -63, true, false },
        { "CoffeeShop-Free", -71, false, false },
        { "Company-Corp-WiFi", -75, true, true },
        { "Neighbor_5F", -80, true, false },
    };
    int cnt = (int)(sizeof(demo) / sizeof(demo[0]));
    if (cnt > max) cnt = max;
    for (int i = 0; i < cnt; i++) {
        strlcpy(out[i].ssid, demo[i].ssid, sizeof(out[i].ssid));
        out[i].rssi = demo[i].rssi;
        out[i].secure = demo[i].secure;
        out[i].enterprise = demo[i].ent;
    }
    *n = cnt;
    return 0;
}

int ds_wifi_connect(const char *ssid, const char *pass)
{
    const struct timespec ts = { 1, 200 * 1000 * 1000 }; /* 模拟连接耗时 */
    nanosleep(&ts, NULL);
    const char *env = getenv("GADGET_DEMO_WIFI");
    if ((env && strcmp(env, "connect_fail") == 0) ||
        (pass && strcmp(pass, "wrong") == 0)) {
        return -1;
    }
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    s_connected = true;
    return 0;
}

bool ds_wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
    if (ssid_cap > 0) strlcpy(ssid, s_connected ? s_cur_ssid : "", (size_t)ssid_cap);
    if (ip_cap > 0) strlcpy(ip, s_connected ? "192.168.1.42" : "", (size_t)ip_cap);
    return s_connected;
}

bool ds_bridge_host_get(char *out, int cap)
{
    if (s_saved_host[0]) {
        snprintf(out, (size_t)cap, "%s", s_saved_host);
        return true;
    }
    const char *env = getenv("GADGET_BRIDGE_URL"); /* 与 ds_init 同源 */
    if (env && env[0]) {
        snprintf(out, (size_t)cap, "%s", env);
        return true;
    }
    out[0] = '\0';
    return false;
}

void ds_bridge_host_save(const char *url)
{
    strlcpy(s_saved_host, url ? url : "", sizeof(s_saved_host));
    for (int i = 0; i < s_host_book_n; i++) {
        if (strcmp(s_host_book[i], url) == 0) return;
    }
    if (s_host_book_n < 4 && url && url[0]) {
        strlcpy(s_host_book[s_host_book_n++], url, sizeof(s_host_book[0]));
    }
}

int ds_bridge_host_list(char *out, int cap)
{
    if (s_host_book_n == 0) {
        if (ds_bridge_host_get(out, cap)) return 0;
        out[0] = '\0';
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < s_host_book_n; i++) {
        int w = snprintf(out + off, (size_t)cap - off, "%s%s",
                         i ? "\n" : "", s_host_book[i]);
        if (w <= 0 || (size_t)w >= (size_t)cap - off) break;
        off += (size_t)w;
    }
    return 0;
}

int ds_bridge_host_remove(const char *url)
{
    if (!url || (s_saved_host[0] && strcmp(s_saved_host, url) == 0)) return -1;
    for (int i = 0; i < s_host_book_n; i++) {
        if (strcmp(s_host_book[i], url) == 0) {
            for (int j = i; j < s_host_book_n - 1; j++) {
                strlcpy(s_host_book[j], s_host_book[j + 1], sizeof(s_host_book[0]));
            }
            s_host_book_n--;
            return 0;
        }
    }
    return -1;
}

#endif /* ESP_PLATFORM */
