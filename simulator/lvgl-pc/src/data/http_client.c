#include "http_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* 解析 http://host[:port]/path */
static int parse_url(const char *url, char *host, size_t host_len, int *port, char *path, size_t path_len)
{
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    if (!slash) return -1;

    size_t hostlen = (size_t)(slash - p);
    if (hostlen == 0 || hostlen >= host_len) return -1;
    memcpy(host, p, hostlen);
    host[hostlen] = '\0';

    *port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0) return -1;
    }

    if (strlen(slash) >= path_len) return -1;
    strcpy(path, slash);
    return 0;
}

static void set_timeouts(int fd, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int http_request(const char *method, const char *url, const char *json_body,
                        char *out, size_t out_len, int timeout_ms, size_t *body_len_out);

int http_get(const char *url, char *out, size_t out_len, int timeout_ms)
{
    return http_request("GET", url, NULL, out, out_len, timeout_ms, NULL);
}

/* 二进制安全 GET：真实 body 字节数写 *body_len（不可为 NULL）。
 * out 不以 NUL 终止语义消费（仍会写终止符，但二进制中含 0x00）。 */
int http_get_bin(const char *url, char *out, size_t out_len, size_t *body_len,
                 int timeout_ms)
{
    return http_request("GET", url, NULL, out, out_len, timeout_ms, body_len);
}

int http_post_json(const char *url, const char *body, char *out, size_t out_len, int timeout_ms)
{
    return http_request("POST", url, body, out, out_len, timeout_ms, NULL);
}

/* ---- 流式 POST（SSE 用） ---- */

/* 连接并发出 POST；返回 fd（调用方负责 close），负值=错误。 */
static int connect_and_send(const char *url, const char *body, int timeout_ms,
                            char *host, size_t host_len, char *path, size_t path_len)
{
    int port;
    if (parse_url(url, host, host_len, &port, path, path_len) != 0) return -1;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -2;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -3;
    }
    set_timeouts(fd, timeout_ms);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -4;
    }
    freeaddrinfo(res);

    size_t body_len = strlen(body);
    char req[1024];
    int reqlen = snprintf(req, sizeof(req),
                          "POST %s HTTP/1.1\r\n"
                          "Host: %s:%d\r\n"
                          "Accept: text/event-stream, application/json\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          path, host, port, body_len);
    if (reqlen < 0 || write(fd, req, (size_t)reqlen) != reqlen ||
        write(fd, body, body_len) != (ssize_t)body_len) {
        close(fd);
        return -5;
    }
    return fd;
}

/* 从 fd 逐字节读到行尾（去掉 \r\n）；返回 -1=EOF，否则为行长度（0=空行）。 */
static ssize_t read_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    bool got_any = false;
    for (;;) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return got_any ? (ssize_t)n : -1;
        got_any = true;
        if (c == '\n') break;
        if (c != '\r') {
            if (n < cap - 1) buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

static ssize_t read_exact(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* chunked 块读取：声明长度可达任意值（服务端决定），必须分片进 8KB
 * 栈缓冲再回调——曾直接 read_exact(fd, chunk[8192], sz) 写爆栈
 * （SIGSEGV，voice/chat 流式路径崩溃的根因） */
static int read_chunked_body(int fd, http_stream_cb_t on_data, void *user)
{
    for (;;) {
        char szline[32];
        if (read_line(fd, szline, sizeof(szline)) <= 0) break;
        unsigned long sz = strtoul(szline, NULL, 16);
        if (sz == 0) break; /* 终止块 */
        if (sz > 64UL * 1024 * 1024) { /* 防恶意/异常声明 */
            return -8;
        }

        char chunk[2048]; /* 勿超 2KB：ESP32 worker 栈预算紧（多次小拷贝换栈安全） */
        while (sz > 0) {
            size_t take = sz < sizeof(chunk) ? sz : sizeof(chunk);
            if (read_exact(fd, chunk, take) < 0) return -7;
            if (on_data) on_data(chunk, take, user);
            sz -= take;
        }
        char crlf[2];
        if (read_exact(fd, crlf, 2) < 0) return -7;
    }
    return 0;
}

int http_post_json_stream(const char *url, const char *body,
                          http_stream_cb_t on_data, void *user, int timeout_ms)
{
    char host[128], path[256];
    int fd = connect_and_send(url, body, timeout_ms, host, sizeof(host), path, sizeof(path));
    if (fd < 0) return fd;

    /* 读 header（逐字节直到空行），判断状态码与 chunked */
    char line[512];
    bool chunked = false;
    int status = 0;
    for (;;) {
        ssize_t ll = read_line(fd, line, sizeof(line));
        if (ll < 0) { close(fd); return -6; } /* header 未完即 EOF */
        if (ll == 0) break;                   /* 空行 = header 结束 */
        if (status == 0 && strncmp(line, "HTTP/1.", 7) == 0) {
            status = atoi(line + 9);
        }
        if (strncasecmp(line, "Transfer-Encoding: chunked", 26) == 0) chunked = true;
    }
    if (status < 200 || status >= 300) {
        close(fd);
        return status; /* 非 2xx 不回调 */
    }

    if (chunked) {
        int rc = read_chunked_body(fd, on_data, user);
        if (rc != 0) { close(fd); return rc; }
    } else {
        char buf[2048]; /* 勿超 2KB：ESP32 worker 栈预算紧 */
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (on_data) on_data(buf, (size_t)n, user);
        }
    }
    close(fd);
    return status;
}

static int http_request(const char *method, const char *url, const char *json_body,
                        char *out, size_t out_len, int timeout_ms, size_t *body_len_out)
{
    char host[128], path[256];
    int port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) return -1;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -2;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -3;
    }
    set_timeouts(fd, timeout_ms);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -4;
    }
    freeaddrinfo(res);

    size_t body_len = json_body ? strlen(json_body) : 0;
    char ct_headers[96] = "";
    if (json_body) {
        snprintf(ct_headers, sizeof(ct_headers),
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n", body_len);
    }
    char req[1024];
    int reqlen = snprintf(req, sizeof(req),
                          "%s %s HTTP/1.1\r\n"
                          "Host: %s:%d\r\n"
                          "Accept: application/json\r\n"
                          "%s"
                          "Connection: close\r\n"
                          "\r\n",
                          method, path, host, port, ct_headers);
    if (reqlen < 0 || write(fd, req, (size_t)reqlen) != reqlen) {
        close(fd);
        return -5;
    }
    if (body_len > 0 && write(fd, json_body, body_len) != (ssize_t)body_len) {
        close(fd);
        return -5;
    }

    /* 读到连接关闭或缓冲满（Connection: close 保证服务端收尾） */
    size_t total = 0;
    char buf[4096];
    ssize_t n;
    while (total < out_len - 1 && (n = read(fd, buf, sizeof(buf))) > 0) {
        if (total + (size_t)n >= out_len) break;
        memcpy(out + total, buf, (size_t)n);
        total += (size_t)n;
    }
    close(fd);
    out[total] = '\0';

    /* 解析状态行 + 去 header（二进制安全：按长度 memmove，不用 strlen） */
    if (total < 12 || strncmp(out, "HTTP/1.", 7) != 0) return -6;
    int status = atoi(out + 9);
    char *resp_body = strstr(out, "\r\n\r\n");
    if (!resp_body) return -7;
    resp_body += 4;
    size_t resp_len = total - (size_t)(resp_body - out);
    if (resp_body != out) memmove(out, resp_body, resp_len);
    out[resp_len] = '\0'; /* 文本调用方兼容；二进制按 *body_len_out 消费 */
    if (body_len_out) *body_len_out = resp_len;
    return status;
}

/* 通用流式 POST：任意 body（带长度）+ 自定义 Content-Type + 响应头捕获。
 * 语音 interact 用：body=PCM，响应=WAV（流回调）+ 文本响应头。 */
int http_post_raw_stream(const char *url, const char *content_type,
                         const char *body, size_t body_len,
                         const char *header1_name, char *header1_value_out, size_t cap1,
                         const char *header2_name, char *header2_value_out, size_t cap2,
                         http_stream_cb_t on_data, void *user, int timeout_ms)
{
    char host[128], path[256];
    int port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) return -1;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -2;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -3;
    }
    set_timeouts(fd, timeout_ms);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -4;
    }
    freeaddrinfo(res);

    char req[1100];
    int reqlen = snprintf(req, sizeof(req),
                          "POST %s HTTP/1.1\r\n"
                          "Host: %s:%d\r\n"
                          "Accept: audio/wav, application/json\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          path, host, port, content_type, body_len);
    if (reqlen < 0 || write(fd, req, (size_t)reqlen) != reqlen ||
        (body_len > 0 && write(fd, body, body_len) != (ssize_t)body_len)) {
        close(fd);
        return -5;
    }

    char line[768];
    bool chunked = false;
    int status = 0;
    size_t name1_len = header1_name ? strlen(header1_name) : 0;
    size_t name2_len = header2_name ? strlen(header2_name) : 0;
    if (header1_value_out && cap1 > 0) header1_value_out[0] = '\0';
    if (header2_value_out && cap2 > 0) header2_value_out[0] = '\0';
    for (;;) {
        ssize_t ll = read_line(fd, line, sizeof(line));
        if (ll < 0) { close(fd); return -6; }
        if (ll == 0) break; /* header 结束 */
        if (status == 0 && strncmp(line, "HTTP/1.", 7) == 0) {
            status = atoi(line + 9);
        }
        if (strncasecmp(line, "Transfer-Encoding: chunked", 26) == 0) chunked = true;
        if (name1_len > 0 && strncasecmp(line, header1_name, name1_len) == 0 &&
            line[name1_len] == ':') {
            const char *v = line + name1_len + 1;
            while (*v == ' ') v++;
            snprintf(header1_value_out, cap1, "%s", v);
        }
        if (name2_len > 0 && strncasecmp(line, header2_name, name2_len) == 0 &&
            line[name2_len] == ':') {
            const char *v = line + name2_len + 1;
            while (*v == ' ') v++;
            snprintf(header2_value_out, cap2, "%s", v);
        }
    }
    if (status < 200 || status >= 300) {
        close(fd);
        return status;
    }

    if (chunked) {
        int rc = read_chunked_body(fd, on_data, user);
        if (rc != 0) { close(fd); return rc; }
    } else {
        char buf[2048]; /* 勿超 2KB：ESP32 worker 栈预算紧 */
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (on_data) on_data(buf, (size_t)n, user);
        }
    }
    close(fd);
    return status;
}
