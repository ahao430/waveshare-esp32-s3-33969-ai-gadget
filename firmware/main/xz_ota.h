/* xz_ota — 小智官方 OTA 注册/激活查询（ADR-014）。
 * 协议逐字段对齐 Bridge Go 实现（internal/xiaozhi/ota.go）与
 * docs/XIAOZHI_DIRECT.md §1：
 *
 *   POST https://api.tenclass.net/xiaozhi/ota/
 *     头 Device-Id/Client-Id/User-Agent/Activation-Version:1
 *     → websocket{url,token} + activation{code}（未绑定时）
 *   POST …/ota/activate body {} → 202 等待绑定 / 200 已绑定
 *
 * 全部阻塞网络调用（TLS 握手 + HTTP），只能在 worker 线程执行
 * （踩坑 #11；栈 ≥12KB，TLS 路径实测需求见 HARDWARE 记录）。 */
#ifndef XZ_OTA_H
#define XZ_OTA_H

#include <stdbool.h>
#include <stddef.h>

#define XZ_OTA_URL_DEFAULT "https://api.tenclass.net/xiaozhi/ota/"

typedef struct {
    bool bound;                /* true=已在官方控制台绑定 */
    char activation_code[12];  /* 未绑定时本次激活码（"123456"） */
    char ws_url[192];          /* WSS 端点（会话用） */
    char ws_token[256];        /* Bearer token（会话用） */
} xz_ota_result_t;

/* 一次 OTA 注册：ws_url/token/bound 写入身份并落 NVS；结果拷到 *out
 * （可 NULL，只要副作用）。返回 0=成功 / -1=失败（err 填原因）。 */
int xz_ota_request(xz_ota_result_t *out, char *err, size_t err_cap);

/* 激活状态查询（供 UI 轮询）：true=已绑定（内部自动 re-OTA 刷新
 * token，Go 版行为）；false=未绑定或网络失败（日志区分）。 */
bool xz_ota_activate_poll(void);

#endif /* XZ_OTA_H */
