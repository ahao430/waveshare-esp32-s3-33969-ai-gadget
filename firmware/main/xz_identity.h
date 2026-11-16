/* xz_identity — 小智直连设备身份（ADR-014，协议见 docs/XIAOZHI_DIRECT.md）。
 *
 * 身份 = Wi-Fi MAC（稳定唯一）+ UUIDv4，首次生成后存 NVS namespace
 * "xiaozhi"；OTA 下发的 ws_url/ws_token/bound 随身份一起持久化（绑定
 * 完成后由 xz_ota re-OTA 刷新 token）。
 *
 * 线程模型：xz_identity_init() 进程级一次（首个调用方负责，当前在
 * xz_ota_request 入口）；此后缓存只读，写回仅发生在 OTA worker（串行）。 */
#ifndef XZ_IDENTITY_H
#define XZ_IDENTITY_H

#include <stdbool.h>

typedef struct {
    char device_id[20];        /* "aa:bb:cc:dd:ee:ff"（Wi-Fi STA MAC） */
    char client_id[40];        /* UUIDv4 */
    char ws_url[192];          /* OTA 下发的 WSS 端点 */
    char ws_token[256];        /* OTA 下发的 Bearer token（只进 NVS 不进日志） */
    char activation_code[12];  /* 最近一次激活码（未绑定时非空） */
    bool bound;                /* 官方控制台已绑定 */
} xz_identity_t;

/* 读取/生成并缓存身份（幂等）。返回 0；NVS 异常时身份仍在内存可用
 * （仅不持久化，日志可辨）。 */
int xz_identity_init(void);

/* 当前身份缓存；未 init 返回 NULL。 */
xz_identity_t *xz_identity(void);

/* 缓存整体写回 NVS（OTA 更新后调用）。0=成功 / -1=NVS 失败。 */
int xz_identity_save(void);

#endif /* XZ_IDENTITY_H */
