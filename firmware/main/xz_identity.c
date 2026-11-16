/* xz_identity — 实现，见 xz_identity.h。 */
#include "xz_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "xz";

static xz_identity_t g_id;
static bool g_ready;

/* UUIDv4：esp_fill_random + RFC 4122 版本/变体位。
 * 官方只要求 Client-Id 唯一稳定，格式不校验（Go 版同款生成策略）。 */
static void gen_uuid4(char *out, size_t cap)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

int xz_identity_init(void)
{
    if (g_ready) return 0;
    memset(&g_id, 0, sizeof(g_id));

    nvs_handle_t h;
    if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "identity: nvs open fail（身份本次不持久化）");
    } else {
        size_t len;
        len = sizeof(g_id.device_id);
        (void)nvs_get_str(h, "device_id", g_id.device_id, &len);
        len = sizeof(g_id.client_id);
        (void)nvs_get_str(h, "client_id", g_id.client_id, &len);
        len = sizeof(g_id.ws_url);
        (void)nvs_get_str(h, "ws_url", g_id.ws_url, &len);
        len = sizeof(g_id.ws_token);
        (void)nvs_get_str(h, "ws_token", g_id.ws_token, &len);
        uint8_t bound = 0;
        (void)nvs_get_u8(h, "bound", &bound);
        g_id.bound = bound != 0;
        nvs_close(h);
    }

    /* 身份缺则生成（真 MAC + UUID）：这台物理设备在官方控制台就是
     * 一台独立设备，与 Bridge 模拟的那台互不相干（ADR-014）。 */
    bool created = false;
    if (g_id.device_id[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(g_id.device_id, sizeof(g_id.device_id),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        created = true;
    }
    if (g_id.client_id[0] == '\0') {
        gen_uuid4(g_id.client_id, sizeof(g_id.client_id));
        created = true;
    }
    g_ready = true;
    if (created) (void)xz_identity_save();
    ESP_LOGI(TAG, "identity: dev=%s bound=%d ws=%d", g_id.device_id,
             g_id.bound, g_id.ws_url[0] != '\0');
    return 0;
}

xz_identity_t *xz_identity(void)
{
    return g_ready ? &g_id : NULL;
}

int xz_identity_save(void)
{
    nvs_handle_t h;
    if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) return -1;
    (void)nvs_set_str(h, "device_id", g_id.device_id);
    (void)nvs_set_str(h, "client_id", g_id.client_id);
    (void)nvs_set_str(h, "ws_url", g_id.ws_url);
    (void)nvs_set_str(h, "ws_token", g_id.ws_token);
    (void)nvs_set_u8(h, "bound", g_id.bound ? 1 : 0);
    int rc = nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
}
