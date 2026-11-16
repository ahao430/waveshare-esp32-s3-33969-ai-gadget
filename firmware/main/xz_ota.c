/* xz_ota — 实现，见 xz_ota.h。 */
#include "xz_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "xz_identity.h"

static const char *TAG = "xz";

/* 上报板型/版本（官方只校验非空；不伪装具体 Waveshare 出厂型号，
 * 避免与真实出厂设备在控制台里混淆——Go 版同策略） */
#define XZ_BOARD  "ai-gadget-33969"
#define XZ_APPVER "1.0.0"

/* 带 identity 头的阻塞 POST。返回 HTTP 状态码（<0=传输失败）；
 * 响应体读入 resp（NUL 结尾），*resp_len 填长度。 */
static int ota_post(const char *url, const char *body,
                    char *resp, size_t resp_cap, size_t *resp_len)
{
    xz_identity_t *id = xz_identity();
    if (!id) return -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach, /* 内置 Mozilla bundle */
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        ESP_LOGE(TAG, "ota: client init fail");
        return -2;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Device-Id", id->device_id);
    esp_http_client_set_header(h, "Client-Id", id->client_id);
    esp_http_client_set_header(h, "User-Agent", XZ_BOARD "/" XZ_APPVER);
    esp_http_client_set_header(h, "Activation-Version", "1");
    esp_http_client_set_header(h, "Accept-Language", "zh-CN");

    int status = -1;
    if (esp_http_client_open(h, (int)strlen(body)) == ESP_OK) {
        int w = esp_http_client_write(h, body, (int)strlen(body));
        if (w >= 0) {
            (void)esp_http_client_fetch_headers(h); /* <0=chunked，仍可读 */
            int total = 0;
            while (total < (int)resp_cap - 1) {
                int r = esp_http_client_read(h, resp + total,
                                             (int)resp_cap - 1 - total);
                if (r <= 0) break;
                total += r;
            }
            resp[total] = '\0';
            *resp_len = (size_t)total;
            status = esp_http_client_get_status_code(h);
        }
    }
    if (status < 0) {
        ESP_LOGE(TAG, "ota post: %s status=%d", url, status);
    }
    esp_http_client_cleanup(h);
    return status;
}

/* OTA 系统信息上报体（字段名对齐 Go otaRequest/上游 ota.cc；
 * features 全 false——不实现 MCP/UDP/AEC 就不 advertise） */
static char *ota_body_build(void)
{
    xz_identity_t *id = xz_identity();

    char ssid[33] = "-";
    int rssi = -50, channel = 6;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (ap.ssid[0]) snprintf(ssid, sizeof(ssid), "%s", (const char *)ap.ssid);
        rssi = ap.rssi;
        channel = ap.primary ? ap.primary : channel;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", "zh-CN");
    cJSON_AddStringToObject(root, "mac_address", id->device_id);
    cJSON_AddStringToObject(root, "uuid", id->client_id);
    cJSON *chip = cJSON_AddObjectToObject(root, "chip_info");
    cJSON_AddNumberToObject(chip, "model", 9); /* esp32s3 */
    cJSON_AddNumberToObject(chip, "cores", 2);
    cJSON_AddNumberToObject(chip, "revision", 2);
    cJSON *app = cJSON_AddObjectToObject(root, "application");
    cJSON_AddStringToObject(app, "name", XZ_BOARD);
    cJSON_AddStringToObject(app, "version", XZ_APPVER);
    cJSON_AddStringToObject(app, "idf_version", "v6.0.3");
    cJSON_AddStringToObject(app, "compile_time", __DATE__ " " __TIME__);
    cJSON *board = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(board, "type", XZ_BOARD);
    cJSON_AddStringToObject(board, "name", XZ_BOARD);
    cJSON_AddStringToObject(board, "mac", id->device_id);
    cJSON_AddStringToObject(board, "ssid", ssid);
    cJSON_AddNumberToObject(board, "rssi", rssi);
    cJSON_AddNumberToObject(board, "channel", channel);
    cJSON *proto = cJSON_AddObjectToObject(root, "protocol");
    cJSON *feat = cJSON_AddObjectToObject(proto, "features");
    cJSON_AddBoolToObject(feat, "mcp", false);
    cJSON_AddBoolToObject(feat, "mcp_20260728", false);
    cJSON_AddBoolToObject(feat, "udp_probe", false);
    cJSON_AddBoolToObject(feat, "aec", false);
    cJSON_AddBoolToObject(feat, "glyph_push", false);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt; /* 调用方 cJSON_free */
}

int xz_ota_request(xz_ota_result_t *out, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (!xz_identity() && xz_identity_init() != 0) {
        if (err && err_cap) snprintf(err, err_cap, "identity init fail");
        return -1;
    }

    char *body = ota_body_build();
    if (!body) {
        if (err && err_cap) snprintf(err, err_cap, "body oom");
        return -1;
    }
    char *resp = malloc(4096); /* OTA 响应 <2KB；≥16KB 自动落 PSRAM */
    if (!resp) {
        cJSON_free(body);
        if (err && err_cap) snprintf(err, err_cap, "resp oom");
        return -1;
    }

    size_t rlen = 0;
    int st = ota_post(XZ_OTA_URL_DEFAULT, body, resp, 4096, &rlen);
    cJSON_free(body);
    if (st != 200) {
        if (err && err_cap) snprintf(err, err_cap, "http %d: %.96s", st, resp);
        free(resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        if (err && err_cap) snprintf(err, err_cap, "bad json");
        return -1;
    }
    const cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    const cJSON *url = ws ? cJSON_GetObjectItem(ws, "url") : NULL;
    const cJSON *tok = ws ? cJSON_GetObjectItem(ws, "token") : NULL;
    if (!cJSON_IsString(url) || !url->valuestring[0] ||
        !cJSON_IsString(tok) || !tok->valuestring[0]) {
        cJSON_Delete(root);
        if (err && err_cap) snprintf(err, err_cap, "no websocket.url");
        return -1;
    }

    xz_identity_t *id = xz_identity();
    snprintf(id->ws_url, sizeof(id->ws_url), "%s", url->valuestring);
    snprintf(id->ws_token, sizeof(id->ws_token), "%s", tok->valuestring);
    const cJSON *act = cJSON_GetObjectItem(root, "activation");
    const cJSON *code = act ? cJSON_GetObjectItem(act, "code") : NULL;
    if (cJSON_IsString(code) && code->valuestring[0]) {
        snprintf(id->activation_code, sizeof(id->activation_code), "%s",
                 code->valuestring);
        id->bound = false;
    } else {
        id->activation_code[0] = '\0';
        id->bound = true;
    }
    cJSON_Delete(root);
    (void)xz_identity_save();

    if (out) {
        out->bound = id->bound;
        snprintf(out->activation_code, sizeof(out->activation_code), "%s",
                 id->activation_code);
        snprintf(out->ws_url, sizeof(out->ws_url), "%s", id->ws_url);
        snprintf(out->ws_token, sizeof(out->ws_token), "%s", id->ws_token);
    }
    /* token 不打日志；激活码要上屏/告知用户，可以打 */
    ESP_LOGI(TAG, "ota ok: bound=%d code=%s ws=%.60s", id->bound,
             id->activation_code, id->ws_url);
    return 0;
}

bool xz_ota_activate_poll(void)
{
    if (!xz_identity() && xz_identity_init() != 0) return false;

    char url[256], resp[512];
    snprintf(url, sizeof(url), "%s%s", XZ_OTA_URL_DEFAULT, "activate");
    size_t rlen = 0;
    int st = ota_post(url, "{}", resp, sizeof(resp), &rlen);
    if (st == 202) return false; /* 等待用户在官方控制台输入激活码 */
    if (st != 200) {
        ESP_LOGW(TAG, "activate poll: http %d %.64s", st, resp);
        return false;
    }
    xz_identity_t *id = xz_identity();
    if (!id->bound) {
        /* 绑定完成：re-OTA 刷新 token（绑定前后可能不同，Go 版行为） */
        char err[96];
        xz_ota_result_t r;
        if (xz_ota_request(&r, err, sizeof(err)) == 0) {
            ESP_LOGI(TAG, "activated: re-ota ok");
        } else {
            ESP_LOGW(TAG, "activated but re-ota fail: %s", err);
        }
    }
    return true;
}
