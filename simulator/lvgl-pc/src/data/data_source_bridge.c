/* bridge 实现：HTTP GET Go Bridge 的额度快照接口。
 * 对应 Bridge API：GET /api/v1/quota/{provider}/snapshot（internal/api）。
 * 说明：Phase 1 只映射额度窗口；电池/Wi-Fi/服务器数保持 mock 初值
 * （固件上这些来自 power/network 服务）。
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h" /* FetchContent 引入的 DaveGamble/cJSON */
#include "data_source_internal.h"
#include "http_client.h"

/* 语音链路排障：请求发出/返回（含 HTTP 状态与耗时）必须留痕——
 * 之前"按住说话没反应"只能看到状态机 15s 超时，看不到卡在哪一步 */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#define DSLOG(fmt, ...) ESP_LOGI("ds", fmt, ##__VA_ARGS__)
#define DSNOW_MS() ((long long)(esp_timer_get_time() / 1000))
#else
#define DSLOG(fmt, ...) fprintf(stderr, "[ds] " fmt "\n", ##__VA_ARGS__)
#define DSNOW_MS() 0LL
#endif

#define BRIDGE_TIMEOUT_MS 1500
/* Komari 聚合要等 Bridge 先去抓上游（实测 1.3~1.5s），给足余量；
 * 1500ms 会让监控页永远是空的（2026-11-15 用户反馈）。 */
#define BRIDGE_BUF_SIZE   (64 * 1024)


/* ---------------- TF 离线缓存（ADR-010，2026-11-16） ----------------
 * 抓取成功后整包 JSON 落 <SD>/cache/<file>；网络失败时回读缓存解析——
 * 断网也能显示最后一次数据（订阅的 updated 时间用文件 mtime）。
 * 未插卡/读写失败全程静默：缓存是增强项，不是依赖项。 */
static void cache_write(const char *file, const char *body)
{
    char path[192];
    if (ds_sd_resolve(path, (int)sizeof(path), file) != 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    size_t n = strlen(body);
    (void)fwrite(body, 1, n, f);
    fclose(f);
}

/* 读缓存到 buf（NUL 结尾）；返回 0=无缓存 / >0=文件 mtime（unix 秒） */
static long cache_read(const char *file, char *buf, size_t cap)
{
    char path[192];
    if (ds_sd_resolve(path, (int)sizeof(path), file) != 0) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_mtime : (long)time(NULL);
}

static char g_base[160];
static char g_url[256];       /* 主 provider（GADGET_BRIDGE_PROVIDER） */
static char g_kimi_url[256];
static char g_prewarm_url[256];
static char g_providers_url[256];
static char g_voice_url[256];
static char g_chat_url[256];
static char g_chat_reset_url[256];
static char g_subs_url[256];
static char g_devcfg_url[256];
static char g_name[176]; /* "bridge(" + g_base(160) + ")" 余量 */
static char g_provider[24];

/* 前置声明：bridge_voice_interact / bridge_fetch_background 函数体在文件尾部 */
static ds_voice_result_t bridge_voice_interact(const char *app, const int16_t *pcm, int frames,
                                               char *transcript, int transcript_len,
                                               char *reply, int reply_len);
static int bridge_chat_reset(const char *bot_id);
static int bridge_fetch_background(const char *save_path);
static int bridge_fetch_launcher(char *out_csv, int cap, int *count);
static int bridge_fetch_weather(ds_weather_t *out);
static int bridge_fetch_theme(int *theme);
static int bridge_fetch_keys(char *pwr_short, char *boot_short,
                             char *key3_short, char *key3_long, int cap);

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

static void copy_window(const cJSON *win, int *used, long *reset, int *win_s)
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
    if (win_s) {
        const cJSON *w = cJSON_GetObjectItem(win, "window_seconds");
        if (cJSON_IsNumber(w) && w->valuedouble > 0) *win_s = (int)w->valuedouble;
    }
}

static bool fetch_quota(const char *url, int *used_5h, long *reset_5h,
                        int *used_week, char *plan, size_t plan_len,
                        long *fetched_ts, int *win_5h_s, int *week_s,
                        long *reset_week)
{
    /* 64KB 响应缓冲严禁上栈（ESP32 worker 栈 8-16KB 会炸穿——真机
     * 随机堆损坏的根源）；malloc 后解析完立即释放。≥16KB 在 IDF
     * 自动落 PSRAM，PC 走堆。 */
    char *buf = malloc(BRIDGE_BUF_SIZE);
    if (!buf) return false;
    int status = http_get(url, buf, BRIDGE_BUF_SIZE, BRIDGE_TIMEOUT_MS);
    cJSON *root = (status == 200) ? cJSON_Parse(buf) : NULL;
    free(buf);
    if (!root) return false;

    bool ok = false;
    do {
        const cJSON *snap = cJSON_GetObjectItem(root, "snapshot");
        if (!cJSON_IsObject(snap)) break;

        const cJSON *windows = cJSON_GetObjectItem(snap, "windows");
        if (!cJSON_IsArray(windows)) break;

        copy_window(find_window(windows, "5h"), used_5h, reset_5h, win_5h_s);
        if (used_week) copy_window(find_window(windows, "weekly"), used_week, reset_week, week_s);

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

/* 拉取订阅实例聚合快照（GET /api/v1/quota/subscriptions）：
 * 动态填充 st->subs[]（数量由配置决定，不再写死智谱/Kimi）。
 * 快照窗口映射：5h/weekly → 设备展示字段；credits → 余额副行。 */
static void fetch_subscriptions(gadget_state_t *st)
{
    DSLOG("subs fetch: start url=%s", g_subs_url);
    char *buf = malloc(BRIDGE_BUF_SIZE); /* 栈上 64KB 会炸穿 worker 栈 */
    if (!buf) {
        DSLOG("subs fetch: buf alloc failed (%d B)", BRIDGE_BUF_SIZE);
        return;
    }
    int status = http_get(g_subs_url, buf, BRIDGE_BUF_SIZE, BRIDGE_TIMEOUT_MS * 4);
    long cache_mt = 0;
    cJSON *root = NULL;
    if (status == 200) {
        root = cJSON_Parse(buf);
        if (root) cache_write("cache/subscriptions.json", buf);
        else DSLOG("subs fetch: HTTP 200 但 JSON 解析失败");
    }
    if (!root) { /* 断网/解析失败：回读 TF 离线缓存（ADR-010） */
        cache_mt = cache_read("cache/subscriptions.json", buf, BRIDGE_BUF_SIZE);
        if (cache_mt) root = cJSON_Parse(buf);
        /* 失败路径此前完全静默——真机"订阅页空"无从定位，补日志 */
        DSLOG("subs fetch: status=%d cache=%s", status,
              cache_mt ? (root ? "hit" : "hit-parse-fail") : "miss");
    }
    free(buf);
    if (!root) return;
    do {
        const cJSON *arr = cJSON_GetObjectItem(root, "subscriptions");
        if (!cJSON_IsArray(arr)) break;

        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (n >= DS_SUB_MAX) break;
            const cJSON *sid = cJSON_GetObjectItem(it, "id");
            const cJSON *label = cJSON_GetObjectItem(it, "label");
            const cJSON *cpw = cJSON_GetObjectItem(it, "can_prewarm");
            ds_subscription_t *s = &st->subs[n];
            memset(s, 0, sizeof(*s));
            if (cJSON_IsString(label) && label->valuestring[0]) {
                strncpy(s->name, label->valuestring, sizeof(s->name) - 1);
            } else if (cJSON_IsString(sid)) {
                strncpy(s->name, sid->valuestring, sizeof(s->name) - 1);
            }
            s->can_prewarm = cJSON_IsBool(cpw) && cJSON_IsTrue(cpw);
            const cJSON *snap = cJSON_GetObjectItem(it, "snapshot");
            if (!cJSON_IsObject(snap)) {
                s->valid = false; /* 抓取失败/无 Key：显示错误态 */
                snprintf(s->plan, sizeof(s->plan), "抓取失败");
                const cJSON *err = cJSON_GetObjectItem(it, "error");
                if (cJSON_IsString(err) && err->valuestring[0]) {
                    snprintf(s->plan, sizeof(s->plan), "%.20s", err->valuestring);
                }
                n++;
                continue;
            }
            const cJSON *plan = cJSON_GetObjectItem(snap, "plan_name");
            if (cJSON_IsString(plan)) {
                strncpy(s->plan, plan->valuestring, sizeof(s->plan) - 1);
            }
            /* 余额型（DeepSeek 等按量计费）：credits 首条 → 余额文本。
             * 不显示货币符号：montserrat 字体无 ¥（U+00A5）会渲染成豆腐块；
             * 单位是 CNY 时只留数字（副题"账户余额"已表意），其他币种
             * 用 ASCII 单位前缀。 */
            const cJSON *credits = cJSON_GetObjectItem(snap, "credits");
            const cJSON *c0 = cJSON_IsArray(credits) ? cJSON_GetArrayItem(credits, 0) : NULL;
            if (c0) {
                const cJSON *bal = cJSON_GetObjectItem(c0, "balance");
                const cJSON *unit = cJSON_GetObjectItem(c0, "unit");
                if (cJSON_IsNumber(bal)) {
                    const char *u = cJSON_IsString(unit) ? unit->valuestring : "";
                    if (strcmp(u, "CNY") == 0) u = "";
                    snprintf(s->balance, sizeof(s->balance), "%s%.2f", u, bal->valuedouble);
                }
            }
            const cJSON *windows = cJSON_GetObjectItem(snap, "windows");
            const cJSON *w;
            cJSON_ArrayForEach(w, windows) {
                const cJSON *wid = cJSON_GetObjectItem(w, "id");
                const cJSON *up = cJSON_GetObjectItem(w, "used_percent");
                const cJSON *rs = cJSON_GetObjectItem(w, "reset_at");
                const cJSON *ws = cJSON_GetObjectItem(w, "window_seconds");
                long ts = cJSON_IsString(rs) ? parse_iso8601(rs->valuestring) : 0;
                int used = cJSON_IsNumber(up) ? (int)(up->valuedouble + 0.5) : 0;
                int wsec = cJSON_IsNumber(ws) ? (int)ws->valuedouble : 0;
                if (cJSON_IsString(wid) && strcmp(wid->valuestring, "5h") == 0) {
                    s->used_5h = used; s->reset_5h = ts; s->win_5h_s = wsec;
                } else if (cJSON_IsString(wid) && strcmp(wid->valuestring, "weekly") == 0) {
                    s->used_week = used; s->reset_week = ts; s->week_s = wsec;
                }
            }
            s->valid = true;
            n++;
        }
        if (n > 0) st->sub_count = n;
        else st->sub_count = 0; /* 明确空列表 → 设备空态 */
        DSLOG("subs fetch: n=%d", st->sub_count);
        st->updated_unix = cache_mt ? cache_mt : (long)time(NULL);
    } while (0);
    cJSON_Delete(root);
}

/* 设备运行配置下发（订阅间隔/亮度/音量）：GET /api/v1/device/config。
 * 亮度/音量是设备回写的持久化值（可选字段），拿到由调用方应用到
 * 硬件；未下发保持缺省 -1（不覆盖本地值）。失败返回 -1。 */
static int bridge_device_config_fetch(ds_device_config_t *out)
{
    char buf[256];
    if (http_get(g_devcfg_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS) != 200) return -1;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return -1;
    int ret = -1;
    const cJSON *v = cJSON_GetObjectItem(root, "sub_refresh_interval_s");
    if (cJSON_IsNumber(v) && v->valueint >= 60) {
        out->sub_refresh_interval_s = v->valueint;
        ret = 0;
    }
    const cJSON *b = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(b) && b->valueint >= 10 && b->valueint <= 100) {
        out->brightness = b->valueint;
    }
    const cJSON *u = cJSON_GetObjectItem(root, "volume");
    if (cJSON_IsNumber(u) && u->valueint >= 0 && u->valueint <= 100) {
        out->volume = u->valueint;
    }
    cJSON_Delete(root);
    return ret;
}

/* ---------------- 设置回写（亮度/音量跨重启持久化，2026-11-16） ----------------
 * UI 改亮度/音量/息屏超时后调 ds_config_persist（声明在 data_source.h）：
 * 防抖 1.5s 把一轮拖动合并成一次 PUT /api/v1/device/config——Bridge 是
 * 合并语义（PUT 未带的字段保留原值），设备端整体回写当前内存态。
 * 网络在独立 worker（16KB，真机 ENOMEM 回落 12KB，同 ui_worker 档），
 * LVGL 线程直调不阻塞；失败只留日志（持久化是增强项，重启回落默认）。 */
static volatile unsigned g_persist_gen; /* 变更代号：UI 线程 +1，worker 追 */
static volatile bool g_persist_busy;

static void *cfg_persist_worker(void *arg)
{
    (void)arg;
    unsigned seen = g_persist_gen;
    char rbuf[128];
    for (;;) {
        usleep(1500 * 1000); /* 防抖窗：静默 1.5s 才回写 */
        if (g_persist_gen == seen) {
            const gadget_state_t *s = ds_state();
            char body[96];
            snprintf(body, sizeof(body),
                     "{\"sub_refresh_interval_s\":%d,\"brightness\":%d,\"volume\":%d}",
                     s->sub_refresh_interval_s, s->brightness, s->volume);
            int st = http_put_json(g_devcfg_url, body, rbuf, sizeof(rbuf),
                                   BRIDGE_TIMEOUT_MS);
            DSLOG("cfg persist: put=%d bright=%d vol=%d", st, s->brightness, s->volume);
            if (g_persist_gen == seen) break; /* 回写期间无新改动：收工 */
        }
        seen = g_persist_gen; /* 又改了：重新进防抖窗 */
    }
    g_persist_busy = false;
    return NULL;
}

void ds_config_persist(void)
{
    if (g_base[0] == '\0') return; /* mock 模式链接了本文件但未启用 */
    g_persist_gen++;
    if (g_persist_busy) return;    /* worker 在跑：它会发现新代号 */
    g_persist_busy = true;
    pthread_t tid;
    pthread_attr_t attr;
    /* macOS PTHREAD_STACK_MIN=16KB：12KB 档只在 ESP（真机回落）可达 */
    static const size_t k_stacks[] = { 16 * 1024, 12 * 1024 };
    bool ok = false;
    for (int i = 0; i < 2 && !ok; i++) {
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, k_stacks[i]);
        int rc = pthread_create(&tid, &attr, cfg_persist_worker, NULL);
        pthread_attr_destroy(&attr);
        if (rc == 0) {
            pthread_detach(tid);
            ok = true;
        }
    }
    if (!ok) {
        g_persist_busy = false; /* 创建失败：下次改动重试 */
        DSLOG("cfg persist: worker spawn failed");
    }
}

static bool bridge_refresh(gadget_state_t *st)
{
    /* 订阅实例聚合（动态多订阅；无配置 → sub_count=0 → 设备空态）。
     * zhipu/kimi 兼容字段从 subs[0]（若有）派生，供旧引用使用。 */
    fetch_subscriptions(st);
    if (st->sub_count > 0 && st->subs[0].valid) {
        st->zhipu_used_5h   = st->subs[0].used_5h;
        st->zhipu_used_week = st->subs[0].used_week;
        st->zhipu_reset_5h  = st->subs[0].reset_5h;
        st->zhipu_reset_week = st->subs[0].reset_week;
        if (st->zhipu_plan[0] == '\0') {
            snprintf(st->zhipu_plan, sizeof(st->zhipu_plan), "%s", st->subs[0].plan);
        }
    }

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
    const cJSON *s = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(s)) state = s->valuestring;

    /* 从 before/after 快照提取 5h 窗口剩余百分比（弹窗显示"额度 X%→Y%"） */
    double pct_before = -1, pct_after = -1;
    const cJSON *b = cJSON_GetObjectItem(root, "before");
    const cJSON *a = cJSON_GetObjectItem(root, "after");
    const cJSON *wins = b ? cJSON_GetObjectItem(b, "windows") : NULL;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, wins) {
        const cJSON *id = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, "5h") == 0) {
            const cJSON *rp = cJSON_GetObjectItem(it, "remaining_pct");
            if (cJSON_IsNumber(rp)) pct_before = rp->valuedouble;
        }
    }
    wins = a ? cJSON_GetObjectItem(a, "windows") : NULL;
    cJSON_ArrayForEach(it, wins) {
        const cJSON *id = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, "5h") == 0) {
            const cJSON *rp = cJSON_GetObjectItem(it, "remaining_pct");
            if (cJSON_IsNumber(rp)) pct_after = rp->valuedouble;
        }
    }
    const cJSON *jmsg = cJSON_GetObjectItem(root, "message");
    const char *wmsg = (cJSON_IsString(jmsg) && jmsg->valuestring[0]) ? jmsg->valuestring : "";
    const cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");

    if (strcmp(state, "executed") == 0) {
        if (pct_before >= 0 && pct_after >= 0) {
            snprintf(msg, (size_t)msg_len,
                     "已发送轻量请求%s\n5h 额度 %d%% → %d%%\n卡片倒计时已刷新",
                     cJSON_IsNumber(dur) ? "" : "", (int)pct_before, (int)pct_after);
        } else {
            snprintf(msg, (size_t)msg_len, "已发送轻量请求\n卡片额度与倒计时已刷新");
        }
    } else if (strcmp(state, "skipped_cooldown") == 0) {
        const cJSON *cd = cJSON_GetObjectItem(root, "cooldown_remain_s");
        int mnt = cJSON_IsNumber(cd) ? (int)(cd->valuedouble / 60) + 1 : 0;
        snprintf(msg, (size_t)msg_len, "冷却中\n约 %d 分钟后可再次预热", mnt);
    } else if (strcmp(state, "skipped_low_quota") == 0) {
        snprintf(msg, (size_t)msg_len, "周额度不足，保护性跳过\n%s", wmsg);
    } else if (strcmp(state, "duplicate") == 0) {
        snprintf(msg, (size_t)msg_len, "重复请求已忽略（幂等）");
    } else if (strcmp(state, "failed") == 0) {
        snprintf(msg, (size_t)msg_len, "轻量请求失败\n%s", wmsg[0] ? wmsg : "见 Bridge 日志");
    } else {
        snprintf(msg, (size_t)msg_len, "预热状态：%s\n%s", state, wmsg);
    }
    cJSON_Delete(root);

    /* 顺带刷新额度，让 UI 立即显示 after 状态 */
    (void)fetch_quota(g_url, &st->zhipu_used_5h, &st->zhipu_reset_5h,
                      &st->zhipu_used_week, st->zhipu_plan,
                      sizeof(st->zhipu_plan), &st->updated_unix, NULL, NULL, NULL);

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

/* AstrBot 机器人实例列表：GET /api/v1/bots → 每行 "id\tlabel"。 */
static int bridge_chat_bots(char *out, int cap)
{
    static char bots_url[256];
    snprintf(bots_url, sizeof(bots_url), "%s/api/v1/bots", g_base);
    char buf[4096];
    if (http_get(bots_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS) != 200) return -1;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return -1;
    size_t used = 0;
    out[0] = '\0';
    const cJSON *bots = cJSON_GetObjectItem(root, "bots");
    const cJSON *it;
    cJSON_ArrayForEach(it, bots) {
        const cJSON *id = cJSON_GetObjectItem(it, "id");
        const cJSON *label = cJSON_GetObjectItem(it, "label");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        int n = snprintf(out + used, (size_t)(cap - (int)used), "%s%s\t%s",
                         used ? "\n" : "", id->valuestring,
                         cJSON_IsString(label) ? label->valuestring : id->valuestring);
        if (n < 0 || used + (size_t)n >= (size_t)cap) break;
        used += (size_t)n;
    }
    cJSON_Delete(root);
    return 0;
}

/* 当前机器人：GET /api/v1/chat/active → {"id":…}。 */
static int bridge_chat_active_id(char *out, int cap)
{
    static char active_url[256];
    snprintf(active_url, sizeof(active_url), "%s/api/v1/chat/active", g_base);
    char buf[1024];
    if (http_get(active_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS) != 200) return -1;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return -1;
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    int ret = 0;
    if (cJSON_IsString(id) && id->valuestring[0]) {
        snprintf(out, (size_t)cap, "%s", id->valuestring);
    } else {
        ret = 1; /* 未配置实例 */
    }
    cJSON_Delete(root);
    return ret;
}

/* 切换当前机器人：PUT /api/v1/chat/active {"id":…}。 */
static int bridge_chat_set_active(const char *id)
{
    static char active_url[256];
    snprintf(active_url, sizeof(active_url), "%s/api/v1/chat/active", g_base);
    char body[160], buf[512];
    snprintf(body, sizeof(body), "{\"id\":\"%s\"}", id ? id : "");
    int st = http_put_json(active_url, body, buf, sizeof(buf), BRIDGE_TIMEOUT_MS);
    return (st == 200) ? 0 : -1;
}

/* urls_fill 由 g_base 重算全部模块级 URL（注册与 rebind 共用）。
 * 配网页改 Bridge 地址后经 ds_bridge_rebind 立即生效，无需重启。 */
static void urls_fill(void)
{
    snprintf(g_url, sizeof(g_url), "%s/api/v1/quota/%s/snapshot",
             g_base, g_provider);
    snprintf(g_kimi_url, sizeof(g_kimi_url), "%s/api/v1/quota/kimi/snapshot", g_base);
    snprintf(g_prewarm_url, sizeof(g_prewarm_url), "%s/api/v1/quota/zhipu/prewarm", g_base);
    snprintf(g_voice_url, sizeof(g_voice_url), "%s/api/v1/voice/interact", g_base);
    snprintf(g_providers_url, sizeof(g_providers_url), "%s/api/v1/chat/providers", g_base);
    snprintf(g_chat_reset_url, sizeof(g_chat_reset_url), "%s/api/v1/chat/reset", g_base);
    snprintf(g_chat_url, sizeof(g_chat_url), "%s/api/v1/chat/completions", g_base);
    snprintf(g_subs_url, sizeof(g_subs_url), "%s/api/v1/quota/subscriptions", g_base);
    snprintf(g_devcfg_url, sizeof(g_devcfg_url), "%s/api/v1/device/config", g_base);
}

/* 当前激活的 Bridge 基址（网络诊断等 UI 直接读；mock 模式为空串） */
const char *ds_bridge_active_base(void)
{
    return g_base;
}

void ds_bridge_rebind(const char *bridge_url)
{
    if (!bridge_url || bridge_url[0] == '\0') return;
    strlcpy(g_base, bridge_url, sizeof(g_base));
    snprintf(g_name, sizeof(g_name), "bridge(%s)", g_base);
    urls_fill();
}

const ds_impl_t *ds_bridge_impl(const char *bridge_url, const char *provider)
{
    snprintf(g_base, sizeof(g_base), "%s", bridge_url);
    snprintf(g_provider, sizeof(g_provider), "%s", provider);
    snprintf(g_name, sizeof(g_name), "bridge(%s)", g_base);
    urls_fill();
    static const ds_impl_t impl = {
        .name          = g_name, /* bridge_init 填充后内容稳定，指针不变 */
        .init          = bridge_init,
        .refresh       = bridge_refresh,
        .prewarm_zhipu = bridge_prewarm,
        .chat_send_stream = bridge_chat_stream,
        .voice_interact  = bridge_voice_interact,
        .chat_reset      = bridge_chat_reset,
        .device_config_fetch = bridge_device_config_fetch,
        .chat_providers  = bridge_chat_providers,
        .chat_bots       = bridge_chat_bots,
        .chat_active_id  = bridge_chat_active_id,
        .chat_set_active = bridge_chat_set_active,
        .fetch_background = bridge_fetch_background,
        .fetch_theme      = bridge_fetch_theme,
        .fetch_keys       = bridge_fetch_keys,
        .fetch_weather    = bridge_fetch_weather,
        .fetch_launcher   = bridge_fetch_launcher,
    };
    return &impl;
}

/* 自定义背景：GET /api/v1/appearance/background（≤256KB，Web 端已压到
 * 480×480）。404=未设置（返回 1，保留本地旧文件）；成功写盘返回 0。 */
/* Web 下发主题：GET /api/v1/appearance/theme */
static int bridge_fetch_theme(int *theme)
{
    static char th_url[256];
    snprintf(th_url, sizeof(th_url), "%s/api/v1/appearance/theme", g_base);
    char buf[512];
    int status = http_get(th_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS);
    if (status != 200) return -1;

    /* 响应为 pretty JSON（writeJSON 用 MarshalIndent）——用 cJSON 解析 */
    int ret = 1;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        const cJSON *v = cJSON_GetObjectItem(root, "theme");
        if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint < 4) {
            if (theme) *theme = v->valueint;
            ret = 0;
        }
        cJSON_Delete(root);
    }
    return ret;
}

/* 主页排布：GET /api/v1/launcher（order 数组 → "key,key,..." csv） */
static int bridge_fetch_launcher(char *out_csv, int cap, int *count)
{
    static char lay_url[256];
    snprintf(lay_url, sizeof(lay_url), "%s/api/v1/launcher", g_base);
    char buf[2048];
    int status = http_get(lay_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS);
    if (status != 200) return -1;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return -1;
    int ret = 1, n = 0, pos = 0;
    const cJSON *order = cJSON_GetObjectItem(root, "order");
    if (cJSON_IsArray(order)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, order) {
            if (!cJSON_IsString(it) || !it->valuestring[0]) continue;
            int len = (int)strlen(it->valuestring);
            if (pos + len + 2 >= cap) break;
            pos += snprintf(out_csv + pos, (size_t)(cap - pos), "%s%s",
                            n ? "," : "", it->valuestring);
            n++;
        }
        if (n > 0) {
            if (count) *count = n;
            ret = 0;
        }
    }
    cJSON_Delete(root);
    return ret;
}

/* 天气：GET /api/v1/weather（Bridge 缓存命中即回，Open-Meteo 上游） */
static int bridge_fetch_weather(ds_weather_t *out)
{
    static char wx_url[256];
    snprintf(wx_url, sizeof(wx_url), "%s/api/v1/weather", g_base);
    char *buf = malloc(8192); /* 8KB 也不上栈（worker 栈预算紧） */
    if (!buf) {
        if (out) out->valid = false;
        return -1;
    }
    int status = http_get(wx_url, buf, 8192, BRIDGE_TIMEOUT_MS);
    if (status == 200) {
        cache_write("cache/weather.json", buf);
    } else { /* 断网：回读 TF 离线缓存（ADR-010） */
        if (cache_read("cache/weather.json", buf, 8192) > 0) status = 200;
    }
    if (status != 200) {
        free(buf);
        if (out) out->valid = false;
        return -1;
    }
    if (!out) {
        free(buf);
        return -1;
    }
    memset(out, 0, sizeof(*out));

    int ret = -1;
    cJSON *root = (status == 200) ? cJSON_Parse(buf) : NULL;
    free(buf); /* 解析完成即释放（后续仅用 root） */
    if (!root) return -1;
    const cJSON *snap = cJSON_GetObjectItem(root, "snapshot");
    if (snap) {
        const cJSON *v;
        if ((v = cJSON_GetObjectItem(snap, "location")) && cJSON_IsString(v))
            snprintf(out->location, sizeof(out->location), "%s", v->valuestring);
        const cJSON *cur = cJSON_GetObjectItem(snap, "current");
        if (cur) {
            if ((v = cJSON_GetObjectItem(cur, "temp_c")) && cJSON_IsNumber(v))
                out->temp_c = (int)(v->valuedouble + (v->valuedouble >= 0 ? 0.5 : -0.5));
            if ((v = cJSON_GetObjectItem(cur, "feels_c")) && cJSON_IsNumber(v))
                out->feels_c = (int)(v->valuedouble + (v->valuedouble >= 0 ? 0.5 : -0.5));
            if ((v = cJSON_GetObjectItem(cur, "humidity")) && cJSON_IsNumber(v))
                out->humidity = v->valueint;
            if ((v = cJSON_GetObjectItem(cur, "wind_kmh")) && cJSON_IsNumber(v))
                out->wind_kmh = v->valueint;
            if ((v = cJSON_GetObjectItem(cur, "desc")) && cJSON_IsString(v))
                snprintf(out->desc, sizeof(out->desc), "%s", v->valuestring);
            if ((v = cJSON_GetObjectItem(cur, "category")) && cJSON_IsString(v))
                snprintf(out->category, sizeof(out->category), "%s", v->valuestring);
        }
        const cJSON *daily = cJSON_GetObjectItem(snap, "daily");
        if (cJSON_IsArray(daily)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, daily) {
                if (out->day_n >= 5) break;
                ds_weather_day_t *d = &out->days[out->day_n];
                if ((v = cJSON_GetObjectItem(it, "date")) && cJSON_IsString(v))
                    snprintf(d->date, sizeof(d->date), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(it, "desc")) && cJSON_IsString(v))
                    snprintf(d->desc, sizeof(d->desc), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(it, "t_max")) && cJSON_IsNumber(v))
                    d->tmax = (int)(v->valuedouble + (v->valuedouble >= 0 ? 0.5 : -0.5));
                if ((v = cJSON_GetObjectItem(it, "t_min")) && cJSON_IsNumber(v))
                    d->tmin = (int)(v->valuedouble + (v->valuedouble >= 0 ? 0.5 : -0.5));
                out->day_n++;
            }
        }
        out->valid = out->desc[0] != '\0';
        ret = out->valid ? 0 : -1;
    }
    cJSON_Delete(root);
    return ret;
}

/* Web 下发按键映射：GET /api/v1/keys（keymap 四槽，值域 home/back/
 * none/app/ptt；非法值由 ui_keys_apply_config 忽略） */
static int bridge_fetch_keys(char *pwr_short, char *boot_short,
                             char *key3_short, char *key3_long, int cap)
{
    static char keys_url[256];
    snprintf(keys_url, sizeof(keys_url), "%s/api/v1/keys", g_base);
    char buf[1024];
    int status = http_get(keys_url, buf, sizeof(buf), BRIDGE_TIMEOUT_MS);
    if (status != 200) return -1;

    int ret = 1;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        const cJSON *km = cJSON_GetObjectItem(root, "keymap");
        if (km) {
            const struct { const char *name; char *out; } slots[] = {
                { "pwr_short", pwr_short }, { "boot_short", boot_short },
                { "key3_short", key3_short }, { "key3_long", key3_long },
            };
            int filled = 0;
            for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
                const cJSON *v = cJSON_GetObjectItem(km, slots[i].name);
                if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
                    snprintf(slots[i].out, (size_t)cap, "%s", v->valuestring);
                    filled++;
                }
            }
            ret = filled == 4 ? 0 : 1;
        }
        cJSON_Delete(root);
    }
    return ret;
}

static int bridge_fetch_background(const char *save_path)
{
    static char bg_url[256];
    snprintf(bg_url, sizeof(bg_url), "%s/api/v1/appearance/background", g_base);

    /* 背景图响应缓冲 256KB：ESP 上 PSRAM 惰性分配（DRAM BSS 放不下） */
    static char *buf;
    const size_t buf_cap = 256 * 1024;
    if (buf == NULL) {
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
        buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
#else
        buf = malloc(buf_cap);
#endif
        if (buf == NULL) return -1;
    }
    size_t len = 0;
    int status = http_get_bin(bg_url, buf, buf_cap, &len, BRIDGE_TIMEOUT_MS);
    if (status == 404) return 1;
    if (status != 200 || len == 0) return -1;

    FILE *f = fopen(save_path, "wb");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

/* 语音一步式交互（PLAN §5.3）：POST 原始 PCM（audio/l16;rate=16000），
 * 一次请求同时捕获 X-Voice-Transcript / X-Voice-Reply 文本头；
 * WAV 响应体：真机经 audio_esp32_tts_write 攒 PCM 缓冲（SPEAKING 时
 * ops->play 消费）；模拟器不真播（字幕推进），丢弃。
 * 超时 30s。 */
#ifdef ESP_PLATFORM
#include "voice/audio_esp32.h"

/* 流式语音帧解析（协议见 bridge/internal/voice/stream.go）：
 *   [u8 type][u32 LE len][payload]
 *     type1 PCM（16k/16bit/mono）→ 写播放缓冲（攒够就开播）
 *     type2 最终文本 → 设备字幕/历史
 *     type3 错误文本
 * 兼容旧 Bridge：首帧头不像帧（如 "RIFF"）就整体按裸 WAV 处理。 */
enum { VF_UNKNOWN = 0, VF_FRAMED, VF_RAW };

typedef struct {
    int mode;
    unsigned char hdr[5];
    int hdr_n;
    unsigned char *buf;
    size_t cap, need, got;
    int type;
    char *reply;
    int reply_cap;
    /* 帧统计（POST 完成后打一行，真机排障用） */
    unsigned pcm_frames, pcm_bytes, text_frames, err_frames;
} vframe_t;

static void vf_reset_frame(vframe_t *f)
{
    f->hdr_n = 0;
    f->need = f->got = 0;
    f->type = 0;
}

/* 状态机：hdr_n<5 = 凑帧头；hdr_n==5 且 got<need = 凑载荷（跨包续传）。
 * 关键教训（2026-11-15 真机无声 bug）：一个 PCM 帧分几十个 chunk 到达，
 * got/need 只能在"帧头刚凑满"那一步初始化——之前放在循环开头，每来
 * 一个 chunk 就把 got 清零，帧永远凑不满 → 一帧音频都写不出去、文本
 * 帧也解析不到（表现：rep=0、无语音无文字、播报状态秒退）。 */
static void vf_feed(vframe_t *f, const char *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        if (f->mode == VF_RAW) {
            audio_esp32_tts_write((const char *)p, len);
            return;
        }
        if (f->hdr_n < 5) {
            while (f->hdr_n < 5 && len > 0) {
                f->hdr[f->hdr_n++] = *p++;
                len--;
            }
            if (f->hdr_n < 5) return; /* 头未凑满，等下一包 */

            unsigned int need = (unsigned int)f->hdr[1] |
                                ((unsigned int)f->hdr[2] << 8) |
                                ((unsigned int)f->hdr[3] << 16) |
                                ((unsigned int)f->hdr[4] << 24);
            if (f->mode == VF_UNKNOWN) {
                if (f->hdr[0] >= 1 && f->hdr[0] <= 3 && need <= (4u << 20)) {
                    f->mode = VF_FRAMED;
                    audio_esp32_tts_begin_stream();
                } else {
                    f->mode = VF_RAW; /* 旧版裸 WAV */
                    audio_esp32_tts_write((const char *)f->hdr, 5);
                    continue;
                }
            }
            f->type = f->hdr[0];
            f->need = need;
            f->got = 0; /* 仅帧头刚凑满时初始化 */
        }

        if (f->need > 0) {
            if (f->need > f->cap) {
                unsigned char *nb = realloc(f->buf, f->need);
                if (nb == NULL) {
                    if (f->buf) free(f->buf);
                    memset(f, 0, sizeof(*f)); /* 缓冲不可靠：丢弃本轮 */
                    return;
                }
                f->buf = nb;
                f->cap = f->need;
            }
            size_t rem = f->need - f->got;
            size_t take = rem < len ? rem : len;
            memcpy(f->buf + f->got, p, take);
            f->got += take;
            p += take;
            len -= take;
        }
        if (f->got < f->need) return; /* 载荷未凑满，等下一包 */

        if (f->type == 1 && f->need > 0) {
            f->pcm_frames++;
            f->pcm_bytes += (unsigned)f->need;
            audio_esp32_tts_write_pcm((const char *)f->buf, f->need);
        } else if (f->type == 2 && f->reply && f->reply_cap > 1) {
            f->text_frames++;
            size_t n = f->need < (size_t)(f->reply_cap - 1) ? f->need
                                                            : (size_t)(f->reply_cap - 1);
            memcpy(f->reply, f->buf, n);
            f->reply[n] = '\0';
        } else if (f->type == 3 && f->reply && f->reply_cap > 1) {
            f->err_frames++;
            size_t n = f->need < (size_t)(f->reply_cap - 1) ? f->need
                                                            : (size_t)(f->reply_cap - 1);
            snprintf(f->reply, (size_t)f->reply_cap, "语音请求失败：%.*s", (int)n,
                     (const char *)f->buf);
        }
        vf_reset_frame(f);
    }
}

static void voice_tts_cb(const char *data, size_t len, void *user)
{
    vf_feed((vframe_t *)user, data, len);
}
#else
static void voice_tts_cb(const char *data, size_t len, void *user)
{
    (void)data;
    (void)len;
    (void)user;
}
#endif

/* 云端会话重置：POST /api/v1/chat/reset?bot=<id>（Bridge 轮换该实例的
 * username+session id；bot 空=全部实例）。bot id 字符集（bot-<hex>）无需转义。 */
static int bridge_chat_reset(const char *bot_id)
{
    char buf[128];
    char url[384]; /* reset url(256) + ?bot=(5) + id(64) + 余量 */
    if (bot_id && bot_id[0])
        snprintf(url, sizeof(url), "%s?bot=%.64s", g_chat_reset_url, bot_id);
    else
        snprintf(url, sizeof(url), "%s", g_chat_reset_url);
    DSLOG("chat reset POST %s", url);
    int status = http_post_json(url, "{}", buf, sizeof(buf),
                                BRIDGE_TIMEOUT_MS * 2);
    DSLOG("chat reset done: status=%d", status);
    return status == 200 ? 0 : -1;
}

static ds_voice_result_t bridge_voice_interact(const char *app, const int16_t *pcm, int frames,
                                               char *transcript, int transcript_len,
                                               char *reply, int reply_len)
{
    /* 本地拦截（用户 2026-11-15：识别不到内容就别发云端）：
     * <0.3s 的录音基本是误触——连 ASR 都不打，直接回提示。 */
    if (frames < 4800) {
        snprintf(transcript, (size_t)transcript_len, "……");
        snprintf(reply, (size_t)reply_len, "按住时间太短啦，再试一次。");
        DSLOG("voice skip: frames=%d too short", frames);
        return DS_VOICE_OK;
    }
    if (frames <= 0) {
        snprintf(reply, (size_t)reply_len, "(未录到音频)");
        return DS_VOICE_ERROR;
    }
#ifdef ESP_PLATFORM
    audio_esp32_tts_reset();
#endif
    /* App 标识透传（Bridge 按 App 选 TTS 音色）：?app=chat|xiaozhi|muse|zcode
     * stream=1：Bridge 走帧协议边生成边合成；旧 Bridge 忽略它返回裸 WAV，
     * 设备侧 vf_feed 按首帧头自动回落。缓冲留足查询串，防截断。 */
    char app_url[320];
    if (app && app[0]) {
        snprintf(app_url, sizeof(app_url), "%s?app=%s&stream=1", g_voice_url, app);
    } else {
        snprintf(app_url, sizeof(app_url), "%s?stream=1", g_voice_url);
    }
#ifdef ESP_PLATFORM
    vframe_t vf;
    memset(&vf, 0, sizeof(vf));
    vf.reply = reply;
    vf.reply_cap = reply_len;
#else
    void *vfp = NULL;
#endif
    DSLOG("voice POST %s bytes=%u", app_url, (unsigned)((size_t)frames * 2));
    long long t0 = DSNOW_MS();
    int status = http_post_raw_stream(app_url, "audio/l16;rate=16000",
                                      (const char *)pcm, (size_t)frames * 2,
                                      "X-Voice-Transcript", transcript, (size_t)transcript_len,
                                      "X-Voice-Reply", reply, (size_t)reply_len,
#ifdef ESP_PLATFORM
                                      voice_tts_cb, &vf, 45000);
#else
                                      voice_tts_cb, vfp, 45000);
#endif
#ifdef ESP_PLATFORM
    if (vf.mode == VF_FRAMED) audio_esp32_tts_stream_end();
    DSLOG("voice POST done: status=%d dur=%lldms tr=%u rep=%u "
          "frames[pcm=%u/%uB text=%u err=%u]",
          status, DSNOW_MS() - t0, (unsigned)strlen(transcript), (unsigned)strlen(reply),
          vf.pcm_frames, vf.pcm_bytes, vf.text_frames, vf.err_frames);
    free(vf.buf);
#else
    DSLOG("voice POST done: status=%d dur=%lldms tr=%u rep=%u", status,
          DSNOW_MS() - t0, (unsigned)strlen(transcript), (unsigned)strlen(reply));
#endif
    if (status != 200) {
        snprintf(reply, (size_t)reply_len, "语音请求失败（HTTP %d）", status);
        return DS_VOICE_ERROR;
    }
    return DS_VOICE_OK;
}

