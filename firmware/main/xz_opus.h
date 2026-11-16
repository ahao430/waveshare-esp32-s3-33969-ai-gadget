/* xz_opus — 小智直连 Opus 编解码薄封装（ADR-014）。
 * 16k/mono/16bit/60ms 帧（960 样本），对齐官方 audio_params。
 * esp_audio_codec（≥1.1，含 decoder）统一 API；编解码器状态由组件
 * 内部堆分配（大块自动落 PSRAM）。实例非线程安全：编码归回合驱动
 * 线程，解码归 WS 事件线程，各持一份。 */
#ifndef XZ_OPUS_H
#define XZ_OPUS_H

#include <stddef.h>
#include <stdint.h>

#define XZ_OPUS_SAMPLES 960        /* 60ms @16k */
#define XZ_OPUS_PACKET_MAX 1275    /* RFC 6716 单包上限 */
#define XZ_OPUS_OUT_MAX 5760       /* 解码输出上限 120ms 样本 */

typedef struct xz_opus_enc xz_opus_enc_t;
typedef struct xz_opus_dec xz_opus_dec_t;

xz_opus_enc_t *xz_opus_enc_open(void);
/* 一帧 960 样本 → Opus 包写入 out；返回包长 / -1 失败 */
int xz_opus_enc(xz_opus_enc_t *e, const int16_t *pcm, uint8_t *out, size_t out_cap);
void xz_opus_enc_close(xz_opus_enc_t *e);

xz_opus_dec_t *xz_opus_dec_open(void);
/* 一个 Opus 包 → 16k PCM 写入 out（样本桶）；返回样本数 / -1 失败 */
int xz_opus_dec(xz_opus_dec_t *d, const uint8_t *pkt, size_t len,
                int16_t *out, size_t out_samples);
void xz_opus_dec_close(xz_opus_dec_t *d);

#endif /* XZ_OPUS_H */
