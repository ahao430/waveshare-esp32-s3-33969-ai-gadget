/* xz_opus — 实现，见 xz_opus.h。 */
#include "xz_opus.h"

#include <stdlib.h>

#include "decoder/esp_audio_dec.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/esp_audio_enc.h"
#include "encoder/impl/esp_opus_enc.h"

struct xz_opus_enc {
    void *h;
};
struct xz_opus_dec {
    void *h;
};

/* esp_audio_codec 2.x 走统一分发表：不注册就开不出来
 * （真机首验实证 "AUDIO_DEC: Decoder OPUS(1398100047) not registered"）。
 * 注册幂等，两个 open 都先过一遍。 */
static bool s_opus_reg;

static void opus_register_once(void)
{
    if (s_opus_reg) return;
    s_opus_reg = true;
    esp_opus_enc_register();
    esp_opus_dec_register();
}

xz_opus_enc_t *xz_opus_enc_open(void)
{
    opus_register_once();
    esp_opus_enc_config_t cfg = {
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
        .bitrate = 24000, /* AppVoIP 16k 单声道常用档（Go 版默认同量级） */
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 1, /* 双核 240MHz 余量足，先低档换稳 */
        .enable_fec = false,
        .enable_dtx = false,
    };
    esp_audio_enc_config_t u = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    void *h = NULL;
    if (esp_audio_enc_open(&u, &h) != ESP_AUDIO_ERR_OK) return NULL;
    xz_opus_enc_t *e = malloc(sizeof(*e));
    if (!e) {
        esp_audio_enc_close(h);
        return NULL;
    }
    e->h = h;
    return e;
}

int xz_opus_enc(xz_opus_enc_t *e, const int16_t *pcm, uint8_t *out, size_t out_cap)
{
    if (!e || !pcm || !out) return -1;
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len = XZ_OPUS_SAMPLES * 2,
    };
    esp_audio_enc_out_frame_t of = {
        .buffer = out,
        .len = (uint32_t)out_cap,
    };
    if (esp_audio_enc_process(e->h, &in, &of) != ESP_AUDIO_ERR_OK) return -1;
    return (int)of.encoded_bytes;
}

void xz_opus_enc_close(xz_opus_enc_t *e)
{
    if (!e) return;
    esp_audio_enc_close(e->h);
    free(e);
}

xz_opus_dec_t *xz_opus_dec_open(void)
{
    opus_register_once();
    esp_opus_dec_cfg_t cfg = {
        .sample_rate = 16000,
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false, /* 官方二进制帧 = 裸 Opus 包（v1 协议） */
    };
    esp_audio_dec_cfg_t u = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    void *h = NULL;
    if (esp_audio_dec_open(&u, &h) != ESP_AUDIO_ERR_OK) return NULL;
    xz_opus_dec_t *d = malloc(sizeof(*d));
    if (!d) {
        esp_audio_dec_close(h);
        return NULL;
    }
    d->h = h;
    return d;
}

int xz_opus_dec(xz_opus_dec_t *d, const uint8_t *pkt, size_t len,
                int16_t *out, size_t out_samples)
{
    if (!d || !pkt || !out || len == 0) return -1;
    esp_audio_dec_in_raw_t in = {
        .buffer = (uint8_t *)pkt,
        .len = (uint32_t)len,
    };
    esp_audio_dec_out_frame_t of = {
        .buffer = (uint8_t *)out,
        .len = (uint32_t)(out_samples * 2),
    };
    if (esp_audio_dec_process(d->h, &in, &of) != ESP_AUDIO_ERR_OK) return -1;
    return (int)(of.decoded_size / 2);
}

void xz_opus_dec_close(xz_opus_dec_t *d)
{
    if (!d) return;
    esp_audio_dec_close(d->h);
    free(d);
}
