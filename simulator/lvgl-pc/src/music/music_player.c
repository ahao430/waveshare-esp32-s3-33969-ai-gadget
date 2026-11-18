/* music_player.c — 播放状态机与曲库（双端公共层）。
 * 解码与音频输出在 backend（固件 music_backend_esp32.c / 模拟器
 * music_backend_sim.c），经 ops 注入；UI 只调本层 API（music_player.h）。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_source.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define MLOG(fmt, ...) ESP_LOGI("music", fmt, ##__VA_ARGS__)
#else
#define MLOG(fmt, ...) ((void)0)
#endif
#include "music_player.h"

/* ---------------- backend ops（backend 文件注入） ---------------- */

typedef struct {
    int  (*open_track)(const char *path, int *duration_s, int *sample_rate,
                       char *fmt, int fmt_cap);
    int  (*start)(void);      /* 起解码任务开始输出 */
    void (*pause)(void);
    void (*resume)(void);
    void (*stop)(void);
    void (*spectrum)(uint8_t bars[MUSIC_BARS]); /* 0..255 幅值 */
    void (*get_progress)(int *cur_s, int *total_s);
    int  (*poll_event)(void); /* 1=曲目播完（eos 异步上报，防解码线程死锁） */
} music_backend_ops_t;

/* 双端后端各导出一个 getter；本文件按平台 bind 其中之一 */
int  music_backend_esp32_ops(const music_backend_ops_t **ops);
int  music_backend_sim_ops(const music_backend_ops_t **ops);

/* ---------------- 状态 ---------------- */

#ifndef ESP_PLATFORM
/* 模拟器：无 heap_caps，直接 malloc */
#define heap_caps_malloc(sz, caps) malloc(sz)
#define MALLOC_CAP_SPIRAM 0
#endif

static const music_backend_ops_t *s_ops;
static music_track_t *s_tracks;   /* PSRAM 惰性分配（64 曲 ≈21KB，坑 #19 内部 RAM 账） */
static int s_n;
static int s_cur = -1;
static music_state_t s_state = MUSIC_STOPPED;
static bool s_shuffle;
static music_loop_t s_loop = MUSIC_LOOP_ALL;
static bool s_inited;
/* 最近一次 scan 的空态说明（区分未插卡/只有子目录/没有音频），UI hint 用 */
static char s_scan_note[96];

music_state_t music_player_state(void) { return s_state; }
int music_player_cur_idx(void) { return s_cur; }
bool music_player_get_shuffle(void) { return s_shuffle; }

void music_player_init(void)
{
    if (s_inited) return;
#if defined(ESP_PLATFORM)
    if (music_backend_esp32_ops(&s_ops) != 0) s_ops = NULL;
#else
    if (music_backend_sim_ops(&s_ops) != 0) s_ops = NULL;
#endif
    s_inited = true;
}

void music_player_probe_id3(music_track_t *t);

/* ---------------- 曲库扫描 ---------------- */

void music_player_scan(void)
{
    char root[96];
    int n = 0;

    s_n = 0;
    s_cur = -1;
    s_scan_note[0] = 0;
    if (!s_tracks) {
        s_tracks = heap_caps_malloc(MUSIC_LIST_MAX * sizeof(music_track_t),
                                    MALLOC_CAP_SPIRAM);
    }
    if (!s_tracks) {
        snprintf(s_scan_note, sizeof(s_scan_note), "内存不足，扫描失败");
        return;
    }
    if (ds_sd_resolve(root, sizeof(root), "music") != 0) return;
    /* 64 条 entry ≈7.7KB：放 PSRAM，不占 worker 栈（ui_worker 12KB
     * 回落栈也安全；坑 #13/#26 同思路） */
    ds_file_entry_t *ents = heap_caps_malloc(
        MUSIC_LIST_MAX * sizeof(ds_file_entry_t), MALLOC_CAP_SPIRAM);
    if (!ents) {
        snprintf(s_scan_note, sizeof(s_scan_note), "内存不足，扫描失败");
        return;
    }
    if (ds_files_list(root, ents, MUSIC_LIST_MAX, &n) != 0) {
        /* opendir 失败：未插卡 / 无 music 目录（真机 FATFS） */
        MLOG("scan %s: opendir failed (no card?)", root);
        snprintf(s_scan_note, sizeof(s_scan_note),
                 "未检测到 TF 卡或 music/ 目录\n请插卡后重进曲库");
        free(ents);
        return;
    }

    int dirs = 0, by_ext = 0;
    for (int i = 0; i < n; i++) {
        const char *name = ents[i].name;
        if (ents[i].is_dir) { dirs++; continue; }
        const char *dot = strrchr(name, '.');
        if (!dot) continue;
        const char *ext = dot + 1;
        if (strcasecmp(ext, "mp3") != 0 && strcasecmp(ext, "wav") != 0) {
            by_ext++;
            continue;
        }
        char abspath[256];
        if (ds_sd_resolve(abspath, sizeof(abspath), "music")) continue;
        size_t used = strlen(abspath);
        snprintf(abspath + used, sizeof(abspath) - used, "/%.90s", name);
        music_track_t *t = &s_tracks[s_n];
        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "%s", abspath);
        snprintf(t->fmt, sizeof(t->fmt),
                 strcasecmp(ext, "mp3") == 0 ? "MP3" : "WAV");
        /* 文件名全量拷贝（title 容量=name 容量），再按长度去扩展名——
         * 切点落在 '.'（ASCII 边界），不切半个 UTF-8 中文字符 */
        snprintf(t->title, sizeof(t->title), "%.95s", name);
        size_t tl = strlen(t->title);
        if (strlen(dot) < tl) t->title[tl - strlen(dot)] = 0;
        music_player_probe_id3(t);
        s_n++;
    }
    MLOG("scan %s: %d entries (%d dirs) -> %d tracks (%d by ext)",
         root, n, dirs, s_n, by_ext);
    free(ents);
    if (s_n == 0) {
        if (dirs > 0 && dirs == n)
            snprintf(s_scan_note, sizeof(s_scan_note),
                     "music/ 下只有子目录（不递归）\n请把 MP3/WAV 放在 music/ 根目录");
        else if (n > 0)
            snprintf(s_scan_note, sizeof(s_scan_note),
                     "music/ 下 %d 个文件都不是 MP3/WAV", n - dirs);
        else
            snprintf(s_scan_note, sizeof(s_scan_note),
                     "music/ 目录是空的");
    }
    if (s_n > 0 && s_cur < 0) s_cur = 0;
}

/* 最近一次扫描的空态说明；"" = 非空态或未扫描 */
const char *music_player_scan_note(void) { return s_scan_note; }

/* ---------------- ID3 标签（v2.3/2.4 头部 + v1 尾部） ---------------- */

static void utf16_to_utf8(const uint8_t *src, int n, char *out, int cap)
{
    int o = 0, i = 0;
    if (n >= 2 && src[0] == 0xFF && src[1] == 0xFE) i = 2; /* BOM LE */
    for (; i + 1 < n && o < cap - 4; i += 2) {
        uint32_t u = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8);
        if (u < 0x80) {
            out[o++] = (char)u;
        } else if (u < 0x800) {
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[o] = 0;
}

static void latin1_to_utf8(const uint8_t *src, int n, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < n && o < cap - 2; i++) {
        uint32_t u = src[i];
        if (u < 0x80) {
            out[o++] = (char)u;
        } else {
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[o] = 0;
}

/* 读 ID3 文本帧内容：src[0]=编码字节，后面是文本 */
static void id3_text(const uint8_t *src, int n, char *out, int cap)
{
    if (n <= 1) return;
    switch (src[0]) {
    case 0: latin1_to_utf8(src + 1, n - 1, out, cap); break;
    case 3: snprintf(out, cap, "%s", (const char *)src + 1); break;
    case 1: case 2: utf16_to_utf8(src + 1, n - 1, out, cap); break;
    default: break;
    }
}

static void probe_id3(const char *path, char *title, int tcap,
                      char *artist, int acap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[10];

    /* v2 头部：ID3 ver flags size(syncsafe)；只扫 v2.3/2.4 文本帧 */
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0 &&
        (hdr[3] == 3 || hdr[3] == 4)) {
        int ver = hdr[3];
        int tagsz = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                    ((hdr[8] & 0x7F) << 7) | hdr[9];
        /* 只读标签头 64KB：TIT2/TPE1 通常在 APIC 前；上限 512KB 时
         * 23 首带封面图的 MP3 在 1-bit SDMMC 上要扫十几秒 */
        if (tagsz > (64 << 10)) tagsz = 64 << 10;
        uint8_t *buf = malloc(tagsz);
        if (buf) {
            int got = (int)fread(buf, 1, tagsz, f);
            int pos = 10;
            while (pos + 10 <= got) {
                const uint8_t *fh = buf + pos;
                if (fh[0] < 'A' || fh[0] > 'Z') break; /* 帧尾/填充 */
                int fsz;
                if (ver == 4) {
                    fsz = ((fh[4] & 0x7F) << 21) | ((fh[5] & 0x7F) << 14) |
                          ((fh[6] & 0x7F) << 7) | fh[7];
                } else {
                    fsz = (fh[4] << 24) | (fh[5] << 16) | (fh[6] << 8) | fh[7];
                }
                if (fsz <= 0 || pos + 10 + fsz > got) break;
                if (!memcmp(fh, "TIT2", 4)) id3_text(fh + 10, fsz, title, tcap);
                else if (!memcmp(fh, "TPE1", 4)) id3_text(fh + 10, fsz, artist, acap);
                pos += 10 + fsz;
            }
            free(buf);
        }
    }
    /* v1 尾标签（无 v2 时）：128B 独立缓冲（hdr 只有 10B） */
    uint8_t tail[128];
    if (title[0] == 0 && fseek(f, -128, SEEK_END) == 0 &&
        fread(tail, 1, 128, f) == 128 && !memcmp(tail, "TAG", 3)) {
        latin1_to_utf8(tail + 3, 30, title, tcap);
        latin1_to_utf8(tail + 33, 30, artist, acap);
    }
    fclose(f);
}

/* scan 里调 probe_id3 用；前向声明收拢（见下方公共 API 前） */
void music_player_probe_id3(music_track_t *t)
{
    probe_id3(t->path, t->title, sizeof(t->title),
              t->artist, sizeof(t->artist));
    /* 全空回退文件名（scan 已预填文件名） */
}

/* ---------------- 控制与模式 ---------------- */

int music_player_play(int idx)
{
    if (s_n == 0) return -1;
    if (idx < 0 || idx >= s_n) idx = s_cur >= 0 ? s_cur : 0;
    if (s_ops && s_ops->stop) s_ops->stop();
    if (s_ops && s_ops->open_track &&
        s_ops->open_track(s_tracks[idx].path, &s_tracks[idx].duration_s,
                          NULL, NULL, 0) != 0) {
        s_state = MUSIC_STOPPED;
        return -1;
    }
    if (s_ops && s_ops->start &&
        s_ops->start() != 0) {
        if (s_ops->stop) s_ops->stop(); /* 释放媒体 owner */
        s_state = MUSIC_STOPPED;
        return -1;
    }
    s_cur = idx;
    s_state = MUSIC_PLAYING;
    return 0;
}

void music_player_pause(void)
{
    if (s_state != MUSIC_PLAYING) return;
    if (s_ops && s_ops->pause) s_ops->pause();
    s_state = MUSIC_PAUSED;
}

void music_player_resume(void)
{
    if (s_state != MUSIC_PAUSED) return;
    if (s_ops->resume) s_ops->resume();
    s_state = MUSIC_PLAYING;
}

void music_player_stop(void)
{
    if (s_ops && s_ops->stop) s_ops->stop();
    s_cur = -1;
    s_state = MUSIC_STOPPED;
}

void music_player_next(int dir)
{
    if (s_n == 0) return;
    int idx;
    if (s_shuffle && s_n > 1) {
        do {
            idx = rand() % s_n;
        } while (idx == s_cur);
    } else {
        idx = s_cur < 0 ? 0 : (s_cur + dir + s_n) % s_n;
    }
    music_player_play(idx);
}

void music_player_toggle(void)
{
    if (s_state == MUSIC_PLAYING) music_player_pause();
    else if (s_state == MUSIC_PAUSED) music_player_resume();
    else music_player_play(s_cur);
}

void music_player_set_shuffle(bool on)
{
    s_shuffle = on;
}

void music_player_cycle_loop(void)
{
    s_loop = (s_loop + 1) % (MUSIC_LOOP_OFF + 1);
}

const char *music_player_loop_text(void)
{
    static const char *txt[] = { "列表循环", "单曲循环", "顺序播放" };
    return txt[s_loop];
}

/* backend 报告曲目播完：按模式决定下一首 */
void music_player_on_track_end(void)
{
    if (s_n == 0 || s_cur < 0) return;
    if (s_loop == MUSIC_LOOP_ONE) {
        music_player_play(s_cur);
        return;
    }
    if (s_loop == MUSIC_LOOP_OFF && s_cur == s_n - 1) {
        music_player_stop();
        return;
    }
    music_player_next(+1);
}

/* ---------------- 只读快照（转发 backend） ---------------- */

int music_player_count(void) { return s_n; }

const music_track_t *music_player_track(int idx)
{
    return (idx >= 0 && idx < s_n) ? &s_tracks[idx] : NULL;
}

void music_player_get_progress(int *cur_s, int *total_s)
{
    /* eos 异步轮询：backend 解码线程不得直接调 on_track_end（play 链路里
     * stop 会 join 解码线程，eos 事件在解码线程上下文执行=join 自己死锁，
     * 2026-11-18 模拟器实测）；由 UI timer 经本 API 轮询转发 */
    if (s_ops && s_ops->poll_event && s_ops->poll_event()) {
        music_player_on_track_end();
    }
    if (s_ops && s_ops->get_progress) s_ops->get_progress(cur_s, total_s);
    else { if (cur_s) *cur_s = 0; if (total_s) *total_s = 0; }
}

void music_player_get_spectrum(uint8_t bars[MUSIC_BARS])
{
    if (s_ops && s_ops->spectrum) s_ops->spectrum(bars);
    else memset(bars, 0, MUSIC_BARS);
}

/* ---------------- WAV/RIFF 头解析（双端公共） ---------------- */

/* 打开并遍历 chunk：fmt 采样率/声道/位深 + data 偏移与长度。
 * v1 仅支持 16bit PCM（其他位深/格式返回 -2）；f 最终停在 data 起始 */
int music_riff_parse(FILE *f, int *rate, int *ch, int *bits,
                     long *data_off, long *data_len)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4)) return -1;
    int have_fmt = 0;
    uint8_t ch8[8];
    while (fread(ch8, 1, 8, f) == 8) {
        long sz = (long)(ch8[4] | (long)ch8[5] << 8 | (long)ch8[6] << 16 |
                         (long)ch8[7] << 24);
        long pos = ftell(f);
        if (!memcmp(ch8, "fmt ", 4) && sz >= 16) {
            uint8_t fm[16];
            if (fread(fm, 1, 16, f) != 16) return -1;
            *ch = fm[2] | fm[3] << 8;
            *rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (int)fm[7] << 24;
            *bits = fm[14] | fm[15] << 8;
            have_fmt = 1;
        } else if (!memcmp(ch8, "data", 4)) {
            if (!have_fmt) return -1;
            *data_off = pos;
            if (sz <= 0) {
                fseek(f, 0, SEEK_END);
                *data_len = ftell(f) - pos;
            } else {
                *data_len = sz;
            }
            fseek(f, pos, SEEK_SET);
            return *bits == 16 ? 0 : -2;
        }
        /* 跳过其他 chunk */
        fseek(f, pos + sz + (sz & 1), SEEK_SET);
    }
    return -1;
}
