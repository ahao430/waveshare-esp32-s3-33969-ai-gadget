/* music_backend_sim.c — 模拟器音乐后端：dr_mp3 解码 + SDL 音频输出。
 * esp_audio_codec 是 esp-idf 生态组件模拟器编不了，用 public domain
 * dr_mp3，输出 PCM 契约与固件后端一致（16bit interleaved）。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "music_player.h"

/* 与 music_player.c 的 ops 形状一致（typedef 不外泄，签名兼容） */
typedef struct {
    int  (*open_track)(const char *path, int *duration_s, int *sample_rate,
                       char *fmt, int fmt_cap);
    int  (*start)(void);
    void (*pause)(void);
    void (*resume)(void);
    void (*stop)(void);
    void (*spectrum)(uint8_t bars[MUSIC_BARS]);
    void (*get_progress)(int *cur_s, int *total_s);
    int  (*poll_event)(void);   /* 1=曲目播完（eos 异步上报，防解码线程死锁） */
} music_backend_ops_t;

/* ---------------- 状态 ---------------- */

static pthread_t s_th;
static volatile int s_run;        /* 任务存活 */
static volatile int s_pause;
static volatile int s_quit;       /* 要求任务退出 */
static volatile int s_gen;        /* 代际：stop/切歌后旧任务在途输出作废 */
static volatile int s_cur_sec;
static volatile int s_eos;
static int s_total;
static int s_rate, s_ch;
static int s_is_mp3;
static uint8_t s_bars[MUSIC_BARS];
static float s_lvl[MUSIC_BARS];
static int s_dev;                 /* SDL 音频设备（0=未开） */
static SDL_AudioSpec s_spec;
static drmp3 s_mp3;
static FILE *s_wav;
static long s_wav_left;

static void sim_stop_stream(void);

/* ---------------- SDL 音频设备 ---------------- */

static int audio_open(int rate, int ch)
{
    /* headless 截帧模式跳过 SDL audio（CoreAudio 在 dummy video 下会挂，
     * 2026-11-18 实测 gadget_sim 卡死 5 分钟+）；解码照跑，只是无声 */
    if (getenv("GADGET_SNAP")) return -1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[music-sim] SDL audio init fail %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = rate;
    want.format = AUDIO_S16LSB;
    want.channels = ch;
    want.samples = 2048;
    if (s_dev) SDL_CloseAudioDevice(s_dev);
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_spec, 0);
    if (!s_dev) {
        fprintf(stderr, "[music-sim] open dev fail %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    return 0;
}

static void audio_close(void)
{
    if (s_dev) {
        SDL_ClearQueuedAudio(s_dev);
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
}

/* ---------------- 解码任务 ---------------- */

/* 频谱伪柱：目标随机游走 + 慢衰减（真机为 esp-dsp 真 FFT） */
static void spectrum_tick(void)
{
    for (int i = 0; i < MUSIC_BARS; i++) {
        if (rand() % 7 == 0) s_lvl[i] = 40 + rand() % 216;
        s_lvl[i] *= 0.94f;
        int v = (int)s_lvl[i];
        s_bars[i] = v > 255 ? 255 : (v < 0 ? 0 : v);
    }
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void *dec_thread(void *arg)
{
    (void)arg;
    int gen = s_gen;
    int16_t pcm[2304 * 2];
    long long frames_total = 0;
    long long s_start_us = now_us();
    while (!s_quit) {
        while (s_pause && !s_quit) usleep(50 * 1000);
        if (s_quit || gen != s_gen) break;
        int got = 0;
        int frames = 576;
        if (s_is_mp3) {
            frames = (int)drmp3_read_pcm_frames_s16(&s_mp3, 576, pcm);
            got = frames * s_ch * 2;
            if (gen == s_gen && frames == 0) fprintf(stderr, "[music-sim] mp3 eos at %.1fs\n", (double)frames_total / s_rate);
        } else if (s_wav) {
            got = (int)fread(pcm, 1, sizeof(pcm), s_wav);
            s_wav_left -= got;
            if (s_wav_left <= 0 || got == 0) break;
        } else {
            break;
        }
        if (got <= 0) break;
        /* 实时节流：解码进度不超前真实时间（SDL 设备开着时由队列深度
         * 限速；headless/无声环境用时钟限速，否则 6s 的歌瞬间放完） */
        if (!s_dev && s_rate > 0) {
            long long target_us = (long long)frames_total * 1000000LL /
                                  (s_rate ? s_rate : 44100);
            long long wall_us = now_us() - s_start_us;
            if (target_us > wall_us) usleep((useconds_t)(target_us - wall_us));
        }
        while (!s_quit && s_dev && s_rate > 0 &&
               SDL_GetQueuedAudioSize(s_dev) >
                   (Uint32)(s_rate * s_ch * 2 * 3 / 10)) {
            usleep(20 * 1000);
        }
        if (s_quit || gen != s_gen) break;
        if (s_dev) SDL_QueueAudio(s_dev, pcm, got);
        frames_total += frames;
        s_cur_sec = (int)(frames_total / (s_rate ? s_rate : 44100));
        spectrum_tick();
    }
    if (gen == s_gen && !s_quit) s_eos = 1;
    return NULL;
}

/* ---------------- ops 实现 ---------------- */

static int sim_open_track(const char *path, int *duration_s, int *sample_rate,
                          char *fmt, int fmt_cap)
{
    (void)sample_rate;
    (void)fmt;
    (void)fmt_cap;
    /* 关掉上一首的遗留状态 */
    sim_stop_stream();
    s_cur_sec = 0;
    fprintf(stderr, "[music-sim] open %s\n", path);
    s_is_mp3 = strstr(path, ".mp3") || strstr(path, ".MP3") ||
               (strcasecmp(path + strlen(path) - 4, ".mp3") == 0);
    if (s_is_mp3) {
        if (!drmp3_init_file(&s_mp3, path, NULL)) return -1;
        drmp3_uint64 fc = drmp3_get_pcm_frame_count(&s_mp3);
        s_rate = (int)s_mp3.sampleRate;
        s_ch = s_mp3.channels;
        if (duration_s) *duration_s = (int)(fc / (s_rate ? s_rate : 44100));
        s_total = *duration_s;
    } else {
        s_wav = fopen(path, "rb");
        if (!s_wav) return -1;
        int bits, ch, rate;
        long doff, dlen;
        if (music_riff_parse(s_wav, &rate, &ch, &bits, &doff, &dlen) != 0) {
            fclose(s_wav);
            s_wav = NULL;
            return -1;
        }
        fseek(s_wav, doff, SEEK_SET);
        s_wav_left = dlen;
        s_rate = rate;
        s_ch = ch;
        if (duration_s)
            *duration_s = (int)(dlen / (rate * ch * 2 ? rate * ch * 2 : 1));
        s_total = *duration_s;
    }
    /* 音频设备打开失败（headless/无声环境）不阻止播放——解码照跑，
     * dec_thread 检查 s_dev 跳过输出 */
    audio_open(s_rate, s_ch);
    return 0;
}

static void sim_stop_stream(void)
{
    s_gen++;
    s_quit = 1;
    s_pause = 0;
    if (s_th) {
        pthread_join(s_th, NULL);
        s_th = 0;
    }
    s_quit = 0;
    s_eos = 0;
    memset(s_bars, 0, sizeof(s_bars));
    memset(s_lvl, 0, sizeof(s_lvl));
}

static int sim_start(void)
{
    fprintf(stderr, "[music-sim] start rate=%d ch=%d mp3=%d\n", s_rate, s_ch, s_is_mp3);
    s_gen++;
    s_quit = 0;
    s_pause = 0;
    if (pthread_create(&s_th, NULL, dec_thread, NULL) != 0) {
        s_th = 0;
        return -1;
    }
    return 0;
}

static void sim_pause(void) { s_pause = 1; SDL_PauseAudioDevice(s_dev, 1); }
static void sim_resume(void) { s_pause = 0; SDL_PauseAudioDevice(s_dev, 0); }
static void sim_spectrum(uint8_t bars[MUSIC_BARS]) { memcpy(bars, s_bars, MUSIC_BARS); }

static void sim_get_progress(int *cur_s, int *total_s)
{
    if (cur_s) *cur_s = s_cur_sec;
    if (total_s) *total_s = s_total;
}

static int sim_poll_event(void)
{
    int v = s_eos;
    s_eos = 0;
    return v;
}

int music_backend_sim_ops(const music_backend_ops_t **ops)
{
    static const music_backend_ops_t SIM_OPS = {
        sim_open_track, sim_start, sim_pause, sim_resume,
        sim_stop_stream, sim_spectrum, sim_get_progress, sim_poll_event
    };
    *ops = &SIM_OPS;
    return 0;
}
