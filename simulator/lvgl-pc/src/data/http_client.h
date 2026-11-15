/* 极简阻塞 HTTP/1.1 GET 客户端（仅模拟器联调 Bridge 用；固件用 esp_http_client）。
 * 限制：仅 http://（无 TLS）、单次 header ≤4KB、body ≤256KB、带读写超时。
 * 线程安全：单线程调用（UI 线程 / ds_refresh 内）。
 */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>

/* GET url（形如 http://host:port/path），成功把 body 写入 out。
 * 返回 HTTP 状态码（>=100），失败返回负值（连接/超时/解析错误）。
 * timeout_ms 同时作用于连接、读、写。 */
int http_get(const char *url, char *out, size_t out_len, int timeout_ms);

/* 二进制安全 GET：真实 body 字节数写 *body_len。调用方按长度消费 out
 * （body 内可能含 0x00，不可当 C 字符串）。 */
int http_get_bin(const char *url, char *out, size_t out_len, size_t *body_len,
                 int timeout_ms);

/* POST JSON。body 为请求体（调用方保证是合法 JSON）；其余同 http_get。 */
int http_post_json(const char *url, const char *body, char *out, size_t out_len, int timeout_ms);

/* PUT JSON（同 http_post_json，方法换成 PUT）。 */
int http_put_json(const char *url, const char *body, char *out, size_t out_len, int timeout_ms);

/* POST JSON 流式：响应 body 按 chunked 逐块回调（已解 chunked 帧，
 * data 内容是原始字节）。状态码 >=300 时不回调、返回码；连接/解析
 * 失败返回负值。回调在调用线程同步执行。 */
typedef void (*http_stream_cb_t)(const char *data, size_t len, void *user);
int http_post_json_stream(const char *url, const char *body,
                          http_stream_cb_t on_data, void *user, int timeout_ms);

/* 通用流式 POST：任意 body（带长度）+ 自定义 Content-Type。
 * 捕获两个响应头的值（name2 可为 NULL）；用于语音 interact 一次请求
 * 同时拿 X-Voice-Transcript / X-Voice-Reply。 */
int http_post_raw_stream(const char *url, const char *content_type,
                         const char *body, size_t body_len,
                         const char *header1_name, char *header1_value_out, size_t cap1,
                         const char *header2_name, char *header2_value_out, size_t cap2,
                         http_stream_cb_t on_data, void *user, int timeout_ms);

#endif /* HTTP_CLIENT_H */
