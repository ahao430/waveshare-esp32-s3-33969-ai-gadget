/* xz_session — 实现，见 xz_session.h。
 *
 * 为什么不用 esp_websocket_client（2026-11-16 真机实证）：
 *   它内部任务栈写死 4KB 且走内部 RAM——对话页稳态 largest_int 仅 ~7.6KB
 *   时直接 "Error create websocket task"；即便建起来，WSS 的 TLS 握手在
 *   4KB 栈上必溢出（OTA 实测 TLS 链要 16KB 才稳）。故本模块自己实现
 *   最小 WSS 客户端（esp_tls + RFC6455 帧），整轮同步跑在 16KB 静态栈
 *   语音任务里——TLS 内存走 PSRAM（CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC，
 *   与官方 xiaozhi-esp32 的 S3 配置同款）。
 */
#include "xz_session.h"

#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "voice/audio_esp32.h"
#include "xz_identity.h"
#include "xz_opus.h"

static const char *TAG = "xz";

#define XZ_WAIT_HELLO_MS 10000
#define XZ_TURN_TIMEOUT_MS 60000
#define XZ_PUMP_MS 200
#define XZ_WS_MSG_MAX 8192 /* 分片消息拼装上限；官方单帧均 <1KB */

bool xz_direct_ready(void)
{
    xz_identity_t *id = xz_identity();
    return id && id->bound && id->ws_url[0] && id->ws_token[0];
}

/* ---- 回合状态（单线程：协议驱动与事件处理都在语音任务里） ---- */
typedef struct {
    bool got_hello;
    bool tts_stopped; /* 官方 manual 模式回合结束标志 */
    bool failed;

    char session_id[48];
    char heard[192];  /* stt 最终转写 */
    char reply[768];  /* tts sentence_start 累积 */
    char alert[96];

    xz_opus_dec_t *dec;
    int16_t *pcm; /* 解码缓冲（PSRAM，回合结束释放——曾用 static
                     11.25KB 常驻 .bss 吃掉内部 RAM，2026-11-16） */
    bool tts_begun; /* 首个下行包开流 */
    const xz_turn_cbs_t *cbs;
} xz_turn_t;

typedef struct {
    esp_tls_t *tls;
    int fd;
    xz_turn_t *turn;
    uint8_t *msg; /* 分片拼装（PSRAM） */
    size_t msg_len;
    int msg_op; /* 分片首帧 opcode（1=文本 2=二进制） */
} xz_ws_t;

/* ---- esp_tls 读写小工具 ---- */

/* 有数据/可读返回 0；超时 1；错误 -1。先看 mbedtls 内部缓冲，再 select。 */
static int ws_wait_readable(xz_ws_t *w, int timeout_ms)
{
    if (esp_tls_get_bytes_avail(w->tls) > 0) return 0;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(w->fd, &r);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int s = select(w->fd + 1, &r, NULL, NULL, &tv);
    return s > 0 ? 0 : (s == 0 ? 1 : -1);
}

static int tls_read_all(xz_ws_t *w, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = esp_tls_conn_read(w->tls, (char *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int tls_write_all(xz_ws_t *w, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = esp_tls_conn_write(w->tls, (const char *)buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* 客户端发帧：必须遮罩（RFC6455 §5.3）。上行包都很小（Opus <600B / JSON <1KB）。 */
static int ws_send_frame(xz_ws_t *w, int opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[14];
    size_t hl;
    hdr[0] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        hdr[1] = (uint8_t)(0x80 | len);
        hl = 2;
    } else if (len < 65536) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
        hl = 10;
    }
    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(hdr + hl, mask, 4);
    hl += 4;
    if (tls_write_all(w, hdr, hl) != 0) return -1;
    uint8_t buf[512];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        for (size_t i = 0; i < n; i++) buf[i] = data[off + i] ^ mask[(off + i) & 3];
        if (tls_write_all(w, buf, n) != 0) return -1;
        off += n;
    }
    return 0;
}

/* ---- 下行事件 ---- */

static void turn_on_bin(xz_turn_t *t, const uint8_t *pkt, size_t len)
{
    if (!t->dec || !t->pcm || len == 0) return;
    int n = xz_opus_dec(t->dec, pkt, len, t->pcm, XZ_OPUS_OUT_MAX);
    if (n <= 0) return; /* 坏包跳过，回合不断（Go 版同策略） */
    if (!t->tts_begun) {
        t->tts_begun = true;
        audio_esp32_tts_reset();
        audio_esp32_tts_begin_stream(); /* 攒 1.5s 自动起播+推 SPEAKING */
    }
    audio_esp32_tts_write_pcm((const char *)t->pcm, (size_t)n * 2);
}

static void turn_on_text(xz_turn_t *t, const uint8_t *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!root) return;
    const cJSON *jtype = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(jtype)) {
        cJSON_Delete(root);
        return;
    }
    const char *type = jtype->valuestring;
    const cJSON *jtext = cJSON_GetObjectItem(root, "text");
    const char *text = cJSON_IsString(jtext) ? jtext->valuestring : "";
    if (strcmp(type, "hello") == 0) {
        const cJSON *jsid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(jsid)) {
            snprintf(t->session_id, sizeof(t->session_id), "%s", jsid->valuestring);
        }
        t->got_hello = true; /* transport 校验：官方 PTT 只走 websocket */
    } else if (strcmp(type, "stt") == 0) {
        snprintf(t->heard, sizeof(t->heard), "%s", text);
    } else if (strcmp(type, "tts") == 0) {
        const cJSON *jstate = cJSON_GetObjectItem(root, "state");
        const char *state = cJSON_IsString(jstate) ? jstate->valuestring : "";
        if (strcmp(state, "sentence_start") == 0) {
            size_t used = strlen(t->reply);
            snprintf(t->reply + used, sizeof(t->reply) - used, "%s", text);
            if (t->cbs && t->cbs->on_subtitle) t->cbs->on_subtitle(text, t->cbs->user);
        } else if (strcmp(state, "stop") == 0) {
            t->tts_stopped = true;
        }
    } else if (strcmp(type, "alert") == 0 && t->reply[0] == '\0') {
        snprintf(t->alert, sizeof(t->alert), "%.90s", text);
        t->failed = true;
    }
    cJSON_Delete(root);
}

/* 读并处理一帧。返回 1=已处理，0=超时无数据，-1=断开/协议错误。 */
static int ws_pump(xz_ws_t *w, int timeout_ms)
{
    int wr = ws_wait_readable(w, timeout_ms);
    if (wr == 1) return 0;
    if (wr < 0) return -1;

    uint8_t h[2];
    if (tls_read_all(w, h, 2) != 0) return -1;
    bool fin = h[0] & 0x80;
    int opcode = h[0] & 0x0F;
    bool masked = h[1] & 0x80;
    uint64_t plen = h[1] & 0x7F;
    if (plen == 126) {
        uint8_t e[2];
        if (tls_read_all(w, e, 2) != 0) return -1;
        plen = ((uint64_t)e[0] << 8) | e[1];
    } else if (plen == 127) {
        uint8_t e[8];
        if (tls_read_all(w, e, 8) != 0) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
    }

    uint8_t mask[4] = { 0 };
    if (masked && tls_read_all(w, mask, 4) != 0) return -1;

    uint8_t ctl[125];
    uint8_t *dst;
    if (opcode >= 0x8) { /* 控制帧：独立小缓冲（可插在分片消息中间） */
        if (plen > sizeof(ctl)) return -1;
        dst = ctl;
    } else {
        if (plen > XZ_WS_MSG_MAX || w->msg_len + plen > XZ_WS_MSG_MAX) return -1;
        dst = w->msg + w->msg_len;
    }
    if (plen && tls_read_all(w, dst, (size_t)plen) != 0) return -1;
    if (masked) {
        for (uint64_t i = 0; i < plen; i++) dst[i] ^= mask[i & 3];
    }

    if (opcode == 0x9) { /* ping → pong */
        (void)ws_send_frame(w, 0xA, dst, (size_t)plen);
        return 1;
    }
    if (opcode == 0xA || opcode == 0x8) return opcode == 0xA ? 1 : -1;
    if (opcode != 0 && opcode != 0x1 && opcode != 0x2) return -1;

    if (opcode != 0) w->msg_op = opcode; /* continuation 沿用首帧 opcode */
    w->msg_len += (size_t)plen;
    if (!fin) return 1; /* 等后续分片 */
    if (w->msg_op == 0x2) turn_on_bin(w->turn, w->msg, w->msg_len);
    else turn_on_text(w->turn, w->msg, w->msg_len);
    w->msg_len = 0;
    return 1;
}

/* 等某个回合事件；期间泵下行帧（音频在 pump 里流式播）。
 * 返回 0=事件到达，-1=超时/断开，-2=alert 失败。 */
static int ws_wait(xz_ws_t *w, bool *flag, int timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while (!*flag) {
        if (w->turn->failed) return -2;
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS > (uint32_t)timeout_ms) {
            return -1;
        }
        int r = ws_pump(w, XZ_PUMP_MS);
        if (r < 0) return w->turn->tts_stopped ? 0 : -1;
    }
    return 0;
}

/* ---- 连接：TLS + RFC6455 握手 ---- */
static int ws_open(xz_ws_t *w, const xz_identity_t *id, char *err, size_t err_cap)
{
    const char *url = id->ws_url;
    if (strncmp(url, "wss://", 6) != 0) {
        snprintf(err, err_cap, "ws url not wss");
        return -1;
    }
    const char *host = url + 6;
    const char *slash = strchr(host, '/');
    const char *path = slash ? slash : "/";
    size_t hostlen = slash ? (size_t)(slash - host) : strlen(host);
    char hostbuf[96];
    if (hostlen == 0 || hostlen >= sizeof(hostbuf)) {
        snprintf(err, err_cap, "bad ws url");
        return -1;
    }
    memcpy(hostbuf, host, hostlen);
    hostbuf[hostlen] = '\0';
    int port = 443;
    char *colon = strchr(hostbuf, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    w->tls = esp_tls_init();
    if (!w->tls) {
        snprintf(err, err_cap, "tls oom");
        return -1;
    }
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    if (esp_tls_conn_new_sync(hostbuf, (int)strlen(hostbuf), port, &cfg, w->tls) != 1) {
        snprintf(err, err_cap, "tls connect fail");
        return -1;
    }
    if (esp_tls_get_conn_sockfd(w->tls, &w->fd) != ESP_OK) {
        snprintf(err, err_cap, "tls sockfd fail");
        return -1;
    }
    struct timeval snd = { 5, 0 }; /* 写超时防网络拥塞卡死任务 */
    setsockopt(w->fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    unsigned char key[32];
    size_t klen = 0;
    if (mbedtls_base64_encode(key, sizeof(key), &klen, rnd, sizeof(rnd)) != 0) {
        snprintf(err, err_cap, "ws key fail");
        return -1;
    }
    key[klen] = '\0';

    /* 握手请求/响应复用下行帧缓冲（8KB PSRAM）：TLS 握手栈峰值很高，
     * 别再压 1.5KB 栈上缓冲（16KB 语音任务栈被推过线的嫌疑点，
     * 2026-11-16 崩溃日志实证：FreeRTOS 全局列表被邻接越界写坏）。 */
    char *req = (char *)w->msg;
    int n = snprintf(req, XZ_WS_MSG_MAX,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Authorization: Bearer %s\r\n"
                     "Protocol-Version: 1\r\n"
                     "Device-Id: %s\r\n"
                     "Client-Id: %s\r\n"
                     "User-Agent: xiaozhi-esp32/2.0\r\n"
                     "\r\n",
                     path, hostbuf, (const char *)key, id->ws_token, id->device_id,
                     id->client_id);
    if (n <= 0 || n >= XZ_WS_MSG_MAX) {
        snprintf(err, err_cap, "ws req too big");
        return -1;
    }
    if (tls_write_all(w, req, (size_t)n) != 0) {
        snprintf(err, err_cap, "ws req send fail");
        return -1;
    }

    /* 读 101 响应头（逐字节，遇 \r\n\r\n 止）；请求已发出，缓冲可复用 */
    char *resp = (char *)w->msg;
    int rn = 0;
    while (rn < XZ_WS_MSG_MAX - 1) {
        if (ws_wait_readable(w, 5000) != 0) break;
        ssize_t r = esp_tls_conn_read(w->tls, resp + rn, 1);
        if (r <= 0) break;
        rn++;
        if (rn >= 4 && memcmp(resp + rn - 4, "\r\n\r\n", 4) == 0) break;
    }
    resp[rn] = '\0';
    const char *sp = strchr(resp, ' ');
    if (!sp || strncmp(sp + 1, "101", 3) != 0) {
        /* 只回显状态行（不含 token 等敏感头） */
        char line[96];
        size_t i = 0;
        while (i < sizeof(line) - 1 && resp[i] && resp[i] != '\r') {
            line[i] = resp[i];
            i++;
        }
        line[i] = '\0';
        snprintf(err, err_cap, "ws upgrade %s", line[0] ? line : "fail");
        return -1;
    }
    ESP_LOGI(TAG, "ws connected: %s", hostbuf);
    return 0;
}

static void ws_close(xz_ws_t *w)
{
    if (w->tls) {
        uint8_t code[2] = { 0x03, 0xE8 }; /* 1000 = normal closure */
        (void)ws_send_frame(w, 0x8, code, sizeof(code));
        esp_tls_conn_destroy(w->tls); /* 内部 close socket + 释放 */
        w->tls = NULL;
    }
    if (w->msg) {
        free(w->msg);
        w->msg = NULL;
    }
}

/* 内存水位探针：stack_hwm 为历史最低余量（单调降），ws up 后的值即
 * TLS 握手峰值后的剩余栈；int=max 为内部 RAM 空闲/最大连续块。
 * 附带堆完整性巡检：定位哪一步之后堆被越界写坏（坏块详情随之打印）。 */
static void log_mem(const char *stage)
{
    ESP_LOGI(TAG, "%s: stack_hwm=%uB int=%u/%u psram=%u", stage,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (heap_caps_check_integrity_all(true) != ESP_OK) {
        ESP_LOGE(TAG, "%s: HEAP CORRUPTED（上面已打印坏块详情）", stage);
    }
}

int xz_voice_turn(const int16_t *pcm, int frames, const xz_turn_cbs_t *cbs,
                  char *transcript, int transcript_cap,
                  char *reply, int reply_cap, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    xz_identity_t *id = xz_identity();
    if (!id) {
        if (xz_identity_init() != 0 || !(id = xz_identity())) {
            if (err) snprintf(err, err_cap, "identity fail");
            return -1;
        }
    }
    if (!id->ws_url[0] || !id->ws_token[0]) {
        if (err) snprintf(err, err_cap, "no ws endpoint (ota first)");
        return -1;
    }

    log_mem("turn enter");
    xz_turn_t t = { 0 };
    t.cbs = cbs;
    t.dec = xz_opus_dec_open();
    t.pcm = heap_caps_malloc(XZ_OPUS_OUT_MAX * 2, MALLOC_CAP_SPIRAM);
    if (!t.pcm) t.pcm = malloc(XZ_OPUS_OUT_MAX * 2); /* PSRAM 失败退内部 */
    xz_ws_t w = { 0 };
    w.turn = &t;
    w.fd = -1;
    w.msg = heap_caps_malloc(XZ_WS_MSG_MAX, MALLOC_CAP_SPIRAM);
    if (!w.msg) w.msg = malloc(XZ_WS_MSG_MAX);

    int rc = -1;
    xz_opus_enc_t *enc = NULL;
    if (!t.dec || !t.pcm || !w.msg) {
        if (err) snprintf(err, err_cap, "turn oom");
        goto out;
    }
    if (ws_open(&w, id, err, err_cap) != 0) goto out;
    log_mem("ws up"); /* TLS 握手峰值后的栈/堆余量——16KB 栈够不够看这里 */

    /* 1) hello（features 全 false：不实现 MCP/UDP/AEC 就不 advertise） */
    {
        char hello[256];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                 "\"features\":{\"mcp\":false,\"mcp_20260728\":false,"
                 "\"udp_probe\":false,\"aec\":false},"
                 "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                 "\"channels\":1,\"frame_duration\":60}}");
        if (ws_send_frame(&w, 0x1, (const uint8_t *)hello, strlen(hello)) != 0) {
            if (err) snprintf(err, err_cap, "send hello fail");
            goto out;
        }
    }
    /* 2) 服务端 hello（session_id） */
    if (ws_wait(&w, &t.got_hello, XZ_WAIT_HELLO_MS) != 0) {
        if (err) snprintf(err, err_cap, "server hello timeout");
        goto out;
    }
    log_mem("hello ok");
    /* 3) listen start(manual) → Opus 帧 → listen stop */
    {
        char js[160];
        snprintf(js, sizeof(js),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\","
                 "\"mode\":\"manual\"}",
                 t.session_id);
        if (ws_send_frame(&w, 0x1, (const uint8_t *)js, strlen(js)) != 0) {
            if (err) snprintf(err, err_cap, "send listen fail");
            goto out;
        }
    }
    enc = xz_opus_enc_open();
    if (!enc) {
        if (err) snprintf(err, err_cap, "opus enc oom");
        goto out;
    }
    log_mem("enc open");
    /* 试编码静音帧（不发送）：把 libopus 第一次 process 从 TLS 写中剥出
     * 来——两轮崩溃都卡在 enc open 后，若编码本身越界写坏堆，这里就现形 */
    {
        uint8_t tpkt[XZ_OPUS_PACKET_MAX];
        int16_t tframe[XZ_OPUS_SAMPLES];
        memset(tframe, 0, sizeof(tframe));
        int tp = xz_opus_enc(enc, tframe, tpkt, sizeof(tpkt));
        ESP_LOGI(TAG, "opus selftest: plen=%d", tp);
        log_mem("opus selftest");
    }
    {
        uint8_t pkt[XZ_OPUS_PACKET_MAX];
        int16_t frame[XZ_OPUS_SAMPLES];
        for (int off = 0; off < frames; off += XZ_OPUS_SAMPLES) {
            int take = frames - off < XZ_OPUS_SAMPLES ? frames - off : XZ_OPUS_SAMPLES;
            memcpy(frame, pcm + off, (size_t)take * 2);
            for (int i = take; i < XZ_OPUS_SAMPLES; i++) frame[i] = 0; /* 尾帧补静音 */
            int plen = xz_opus_enc(enc, frame, pkt, sizeof(pkt));
            int fi = off / XZ_OPUS_SAMPLES;
            if (fi < 3 || fi % 20 == 0) {
                ESP_LOGI(TAG, "enc[%d] plen=%d", fi, plen);
                log_mem("enc"); /* 早期帧后查堆：libopus 越界写会现形 */
            }
            if (plen <= 0) continue; /* 单帧编码失败跳过 */
            if (ws_send_frame(&w, 0x2, pkt, (size_t)plen) != 0) break;
            if (ws_pump(&w, 0) < 0) break; /* 上行期间顺带处理 ping/早到事件 */
        }
    }
    xz_opus_enc_close(enc);
    enc = NULL;
    {
        char js[128];
        snprintf(js, sizeof(js),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
                 t.session_id);
        (void)ws_send_frame(&w, 0x1, (const uint8_t *)js, strlen(js));
    }
    /* 4) 等回合结束：tts stop / 出错 / 断开（音频在 pump 里流式播） */
    if (ws_wait(&w, &t.tts_stopped, XZ_TURN_TIMEOUT_MS) != 0) {
        if (t.alert[0]) {
            if (err) snprintf(err, err_cap, "%.90s", t.alert);
        } else if (err && !err[0]) {
            snprintf(err, err_cap, "turn timeout/fail");
        }
        goto out;
    }
    /* 成功：拷贝结果（播放收尾由 audio_esp32 play task 通知） */
    if (transcript && transcript_cap)
        snprintf(transcript, (size_t)transcript_cap, "%s", t.heard);
    if (reply && reply_cap) snprintf(reply, (size_t)reply_cap, "%s", t.reply);
    ESP_LOGI(TAG, "turn ok: heard=%u reply=%u", (unsigned)strlen(t.heard),
             (unsigned)strlen(t.reply));
    rc = 0;

out:
    if (enc) xz_opus_enc_close(enc);
    /* 收尾：tts 开过流则放行播放排空；WS 关闭 */
    if (t.tts_begun) audio_esp32_tts_stream_end();
    ws_close(&w);
    if (t.dec) xz_opus_dec_close(t.dec);
    free(t.pcm);
    log_mem("turn end");
    if (rc != 0) ESP_LOGW(TAG, "turn fail: %s", err ? err : "?");
    return rc;
}