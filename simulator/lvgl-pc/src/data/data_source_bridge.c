/* bridge 实现：HTTP GET Go Bridge 的额度快照接口。
 * 对应 Bridge API：GET /api/v1/quota/{provider}/snapshot（internal/api）。
 * 说明：Phase 1 只映射额度窗口；电池/Wi-Fi/服务器数保持 mock 初值
 * （固件上这些来自 power/network 服务与 Bridge komari 聚合器）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h" /* FetchContent 引入的 DaveGamble/cJSON */
#include "data_source_internal.h"
#include "http_client.h"

#define BRIDGE_TIMEOUT_MS 1500
#define BRIDGE_BUF_SIZE   (64 * 1024)

static char g_base[160];
static char g_url[256];       /* 主 provider（GADGET_BRIDGE_PROVIDER） */
static char g_kimi_url[256];
static char g_komari_url[256];
static char g_prewarm_url[256];
static char g_providers_url[256];
static char g_voice_url[256];
static char g_chat_url[256];
static char g_name[96];
static char g_provider[24];

/* 前置声明：bridge_voice_interact 函数体在文件尾部（追加段） */
static ds_voice_result_t bridge_voice_interact(const int16_t *pcm, int frames,
                                               char *transcript, int transcript_len,
                                               char *reply, int reply_len);

static void bridge_init(void)
{
    snprintf(g_name, sizeof(g_name), "bridge(%s)", g_base);
}

/* RFC3339（含可选小数秒与时区偏移）→ unix 秒；解析失败返回 0 */
static long parse_iso8601(const char *s)
{
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;

    struct tm tmv = {
        .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
        .tm_hour = h, .tm_min = mi, .tm_sec = se,
    };
    long off = 0;
    const char *t = strchr(s, 'T');
    if (t) {
        const char *o = strpbrk(t + 1, "Zz+-"); /* T 之后的首个时区标记 */
        if (o && (*o == '+' || *o == '-')) {
            int oh = 0, om = 0;
            if (sscanf(o + 1, "%d:%d", &oh, &om) >= 1) {
                off = (long)oh * 3600 + (long)om * 60;
                if (*o == '-') off = -off;
            }
        }
    }
    return (long)timegm(&tmv) - off;
}

static const cJSON *find_window(const cJSON *windows, const char *id)
{
    const cJSON *w;
    cJSON_ArrayForEach(w, windows) {
        const cJSON *jid = cJSON_GetObjectItem(w, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) return w;
    }
    return NULL;
}

static void copy_window(const cJSON *win, int *used, long *reset)
{
    if (!win) return;
    if (used) {
        const cJSON *u = cJSON_GetObjectItem(win, "used_percent");
        if (cJSON_IsNumber(u)) *used = (int)(u->valuedouble + 0.5);
    }
    if (reset) {
        const cJSON *r = cJSON_GetObjectItem(win, "reset_at");
        if (cJSON_IsString(r)) {
            long ts = parse_iso8601(r->valuestring);
            if (ts > 0) *reset = ts;
        }
    }
}

static bool fetch_quota(const char *url, int *used_5h, long *reset_5h,
                        int *used_week, char *plan, size_t plan_len,
                        long *fetched_ts)
{
    char buf[BRIDGE_BUF_SIZE];
    int status = http_get(url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS);
    if (status != 200) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;

    bool ok = false;
    do {
        const cJSON *snap = cJSON_GetObjectItem(root, "snapshot");
        if (!cJSON_IsObject(snap)) break;

        const cJSON *windows = cJSON_GetObjectItem(snap, "windows");
        if (!cJSON_IsArray(windows)) break;

        copy_window(find_window(windows, "5h"), used_5h, reset_5h);
        if (used_week) copy_window(find_window(windows, "weekly"), used_week, NULL);

        const cJSON *p = cJSON_GetObjectItem(snap, "plan_name");
        if (plan && cJSON_IsString(p) && p->valuestring[0] != '\0' &&
            strlen(p->valuestring) < plan_len) {
            strcpy(plan, p->valuestring);
        }

        const cJSON *fetched = cJSON_GetObjectItem(snap, "fetched_at");
        if (fetched_ts && cJSON_IsString(fetched)) {
            long ts = parse_iso8601(fetched->valuestring);
            if (ts > 0) *fetched_ts = ts;
        }
        ok = true;
    } while (0);

    cJSON_Delete(root);
    return ok;
}

/* 拉取 Komari 服务器摘要（Bridge 聚合后的 /api/v1/komari/nodes）。
 * 失败静默保留旧值（监控页下次刷新重试）。 */
static void fetch_servers(gadget_state_t *st)
{
    char buf[BRIDGE_BUF_SIZE];
    if (http_get(g_komari_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS) != 200) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    do {
        const cJSON *snap = cJSON_GetObjectItem(root, "snapshot");
        if (!cJSON_IsObject(snap)) break;
        const cJSON *servers = cJSON_GetObjectItem(snap, "servers");
        if (!cJSON_IsArray(servers)) break;

        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, servers) {
            if (n >= DS_SERVER_MAX) break;
            gadget_server_t *s = &st->servers[n];

            const cJSON *name = cJSON_GetObjectItem(it, "name");
            if (cJSON_IsString(name)) {
                strncpy(s->name, name->valuestring, sizeof(s->name) - 1);
                s->name[sizeof(s->name) - 1] = '\0';
            } else {
                snprintf(s->name, sizeof(s->name), "#%d", n + 1);
            }

            const cJSON *on = cJSON_GetObjectItem(it, "online");
            s->online = !cJSON_IsFalse(on);

            const cJSON *cpu = cJSON_GetObjectItem(it, "cpu_pct");
            s->cpu_pct = cJSON_IsNumber(cpu) ? (int)(cpu->valuedouble + 0.5) : 0;
            const cJSON *mem = cJSON_GetObjectItem(it, "mem_pct");
            s->mem_pct = cJSON_IsNumber(mem) ? (int)(mem->valuedouble + 0.5) : 0;

            const cJSON *up = cJSON_GetObjectItem(it, "uptime_s");
            s->uptime_h = cJSON_IsNumber(up) ? (int)(up->valuedouble / 3600.0 + 0.5) : 0;
            n++;
        }
        if (n > 0) st->server_count = n;
    } while (0);

    cJSON_Delete(root);
}

static bool bridge_refresh(gadget_state_t *st)
{
    /* 主 provider → Zhipu 页；失败返回 false（旧数据仍可用） */
    if (!fetch_quota(g_url, &st->zhipu_used_5h, &st->zhipu_reset_5h,
                     &st->zhipu_used_week, st->zhipu_plan,
                     sizeof(st->zhipu_plan), &st->updated_unix)) {
        return false;
    }
    /* kimi provider → Kimi 页；不可用（未配 Key）时静默保留原值 */
    (void)fetch_quota(g_kimi_url, &st->kimi_used_5h, NULL,
                      &st->kimi_used_week, NULL, 0, NULL);

    /* 订阅列表：主 provider → subs[0]，kimi → subs[1]（名称/套餐带入） */
    if (st->sub_count >= 1) {
        st->subs[0].used_5h   = st->zhipu_used_5h;
        st->subs[0].used_week = st->zhipu_used_week;
        st->subs[0].reset_5h  = st->zhipu_reset_5h;
        st->subs[0].reset_week = st->zhipu_reset_week;
        snprintf(st->subs[0].name, sizeof(st->subs[0].name), "%s", g_provider);
        st->subs[0].valid = true;
    }
    if (st->sub_count >= 2) {
        st->subs[1].used_5h   = st->kimi_used_5h;
        st->subs[1].used_week = st->kimi_used_week;
        st->subs[1].valid = true;
    }

    /* Komari 服务器 → 监控页 */
    fetch_servers(st);
    return true;
}

/* 预热（PLAN §5.6）：POST /api/v1/quota/zhipu/prewarm。
 * 幂等 Key 用毫秒时间戳 + 随机数；结果状态与 message 透传给 UI。 */
static ds_prewarm_result_t bridge_prewarm(gadget_state_t *st, char *msg, int msg_len)
{
    char key[48];
    snprintf(key, sizeof(key), "sim-%lld-%d", (long long)time(NULL), rand() % 100000);

    char body[96];
    snprintf(body, sizeof(body), "{\"idempotency_key\":\"%s\"}", key);

    char buf[BRIDGE_BUF_SIZE];
    int status = http_post_json(g_prewarm_url, body, buf, sizeof(buf), BRIDGE_TIMEOUT_MS * 4);
    if (status == 503) {
        snprintf(msg, (size_t)msg_len, "未配置 ZHIPU_API_KEY（provider disabled）");
        return DS_PREWARM_ERROR;
    }
    if (status != 200) {
        snprintf(msg, (size_t)msg_len, "预热请求失败（HTTP %d）", status);
        return DS_PREWARM_ERROR;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        snprintf(msg, (size_t)msg_len, "响应解析失败");
        return DS_PREWARM_ERROR;
    }

    const char *state = "unknown";
    char detail[128] = "";
    const cJSON *s = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(s)) state = s->valuestring;
    const cJSON *m = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(m) && m->valuestring[0]) {
        const cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
        if (cJSON_IsNumber(dur)) {
            snprintf(detail, sizeof(detail), "（%s · %dms）", m->valuestring, dur->valueint);
        } else {
            snprintf(detail, sizeof(detail), "（%s）", m->valuestring);
        }
    }
    cJSON_Delete(root);

    /* 顺带刷新额度，让 UI 立即显示 after 状态 */
    (void)fetch_quota(g_url, &st->zhipu_used_5h, &st->zhipu_reset_5h,
                      &st->zhipu_used_week, st->zhipu_plan,
                      sizeof(st->zhipu_plan), &st->updated_unix);

    snprintf(msg, (size_t)msg_len, "%s%s", state, detail);
    return DS_PREWARM_OK;
}

/* Bigfish Chat 流式：POST stream=true，SSE 增量经 chunked 流回调解析。
 * 超时 60s（LLM 生成较慢）。 */
#define CHAT_TIMEOUT_MS 60000

/* SSE 解析器：跨 chunk 残帧缓冲 + data: 行提取 + delta 回调与拼接 */
typedef struct {
    char buf[8192];
    size_t len;
    ds_chat_delta_cb_t cb;
    void *user;
    char full[4096];   /* 完整回复累积 */
    size_t full_len;
} sse_parser_t;

static void sse_parser_feed(sse_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p->len >= sizeof(p->buf) - 1) { p->len = 0; } /* 溢出保护：丢残帧 */
        p->buf[p->len++] = data[i];
        p->buf[p->len] = '\0';

        if (p->len >= 2 && p->buf[p->len - 2] == '\n' && p->buf[p->len - 1] == '\n') {
            p->buf[p->len - 2] = '\0';
            char *frame = p->buf;
            char *save = NULL;
            for (char *ln = strtok_r(frame, "\n", &save); ln;
                 ln = strtok_r(NULL, "\n", &save)) {
                if (strncmp(ln, "data: ", 6) != 0) continue;
                const char *payload = ln + 6;
                if (strcmp(payload, "[DONE]") == 0) continue;
                cJSON *chunk = cJSON_Parse(payload);
                if (!chunk) continue;
                const cJSON *choices = cJSON_GetObjectItem(chunk, "choices");
                const cJSON *c0 = cJSON_GetArrayItem(choices, 0);
                const cJSON *delta = cJSON_GetObjectItem(c0, "delta");
                const cJSON *content = cJSON_GetObjectItem(delta, "content");
                if (cJSON_IsString(content) && content->valuestring[0] && p->cb) {
                    p->cb(content->valuestring, p->user);
                }
                if (cJSON_IsString(content) &&
                    p->full_len + strlen(content->valuestring) < sizeof(p->full) - 1) {
                    strcpy(p->full + p->full_len, content->valuestring);
                    p->full_len += strlen(content->valuestring);
                }
                cJSON_Delete(chunk);
            }
            p->len = 0;
        }
    }
}

/* http 回调 → 持久 parser */
static void chat_stream_on_data(const char *data, size_t len, void *user)
{
    sse_parser_t *p = user;
    sse_parser_feed(p, data, len);
}

static ds_chat_result_t bridge_chat_stream(const ds_chat_msg_t *msgs, int n,
                                           ds_chat_delta_cb_t on_delta, void *user,
                                           char *reply_full, int reply_len)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", msgs[i].role);
        cJSON_AddStringToObject(m, "content", msgs[i].content);
        cJSON_AddItemToArray(arr, m);
    }
    cJSON *req = cJSON_CreateObject();
    /* 模型选择：UI 下拉（ds_chat_provider()）→ "<provider>/dummy"；
     * 初始值来自 GADGET_CHAT_PROVIDER。TODO：model 名从 Bridge providers
     * 接口携带真实模型名（现各家 OpenAI 兼容端点接受占位）。 */
    char model[64];
    snprintf(model, sizeof(model), "%s/dummy", ds_chat_provider());
    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddBoolToObject(req, "stream", true);
    cJSON_AddItemToObject(req, "messages", arr);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        snprintf(reply_full, (size_t)reply_len, "请求构建失败");
        return DS_CHAT_ERROR;
    }

    sse_parser_t parser = { .cb = on_delta, .user = user };
    int status = http_post_json_stream(g_chat_url, body, chat_stream_on_data, &parser, CHAT_TIMEOUT_MS);
    free(body);

    if (status < 200 || status >= 300) {
        snprintf(reply_full, (size_t)reply_len, "请求失败（HTTP %d）", status);
        return DS_CHAT_ERROR;
    }
    if (parser.full_len == 0) {
        snprintf(reply_full, (size_t)reply_len, "(空回复)");
        return DS_CHAT_ERROR;
    }
    snprintf(reply_full, (size_t)reply_len, "%.*s", (int)parser.full_len, parser.full);
    return DS_CHAT_OK;
}

/* 聊天 provider 列表：GET /api/v1/chat/providers → "\n" 分隔。 */
static int bridge_chat_providers(char *out, int cap)
{
    char buf[4096];
    if (http_get(g_providers_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS) != 200) {
        snprintf(out, (size_t)cap, "mock");
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        snprintf(out, (size_t)cap, "mock");
        return -1;
    }
    const cJSON *arr = cJSON_GetObjectItem(root, "providers");
    size_t used = 0;
    out[0] = '\0';
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it)) {
            int n = snprintf(out + used, (size_t)(cap - (int)used), "%s%s",
                             used ? "\n" : "", it->valuestring);
            if (n < 0 || used + (size_t)n >= (size_t)cap) break;
            used += (size_t)n;
        }
    }
    cJSON_Delete(root);
    return 0;
}

const ds_impl_t *ds_bridge_impl(const char *bridge_url, const char *provider)
{
    snprintf(g_base, sizeof(g_base), "%s", bridge_url);
    snprintf(g_provider, sizeof(g_provider), "%s", provider);
    snprintf(g_url, sizeof(g_url), "%s/api/v1/quota/%s/snapshot",
             bridge_url, provider);
    snprintf(g_kimi_url, sizeof(g_kimi_url), "%s/api/v1/quota/kimi/snapshot",
             bridge_url);
    snprintf(g_komari_url, sizeof(g_komari_url), "%s/api/v1/komari/nodes",
             bridge_url);
    snprintf(g_prewarm_url, sizeof(g_prewarm_url), "%s/api/v1/quota/zhipu/prewarm",
             bridge_url);
    snprintf(g_voice_url, sizeof(g_voice_url), "%s/api/v1/voice/interact",
             bridge_url);
    snprintf(g_providers_url, sizeof(g_providers_url), "%s/api/v1/chat/providers",
             bridge_url);
    snprintf(g_chat_url, sizeof(g_chat_url), "%s/api/v1/chat/completions",
             bridge_url);
    static const ds_impl_t impl = {
        .name          = g_name, /* bridge_init 填充后内容稳定，指针不变 */
        .init          = bridge_init,
        .refresh       = bridge_refresh,
        .prewarm_zhipu = bridge_prewarm,
        .chat_send_stream = bridge_chat_stream,
        .voice_interact  = bridge_voice_interact,
        .chat_providers  = bridge_chat_providers,
    };
    return &impl;
}

/* 语音一步式交互（PLAN §5.3）：POST 原始 PCM（audio/l16;rate=16000），
 * 一次请求同时捕获 X-Voice-Transcript / X-Voice-Reply 文本头；
 * WAV 响应体丢弃（模拟器不真播，字幕推进；真机 Phase 6 交 audio ops）。
 * 超时 30s。 */
static void voice_discard_cb(const char *data, size_t len, void *user)
{
    (void)data;
    (void)len;
    (void)user;
}

static ds_voice_result_t bridge_voice_interact(const int16_t *pcm, int frames,
                                               char *transcript, int transcript_len,
                                               char *reply, int reply_len)
{
    if (frames <= 0) {
        snprintf(reply, (size_t)reply_len, "(未录到音频)");
        return DS_VOICE_ERROR;
    }
    int status = http_post_raw_stream(g_voice_url, "audio/l16;rate=16000",
                                      (const char *)pcm, (size_t)frames * 2,
                                      "X-Voice-Transcript", transcript, (size_t)transcript_len,
                                      "X-Voice-Reply", reply, (size_t)reply_len,
                                      voice_discard_cb, NULL, 30000);
    if (status != 200) {
        snprintf(reply, (size_t)reply_len, "语音请求失败（HTTP %d）", status);
        return DS_VOICE_ERROR;
    }
    return DS_VOICE_OK;
}

