/* music_player — 本地音乐播放器公共层（双端：固件/模拟器，ADR-001）。
 *
 * 职责：播放状态机（STOP/PLAY/PAUSE）、曲目列表与顺序模式（随机/循环）、
 * 控制命令、进度与频谱快照（UI 只读）、ID3 标签解析。
 * 解码与音频输出在 backend（固件 music_backend_esp32 / 模拟器
 * music_backend_sim），经 ops 注入；UI 只调本层 API。
 *
 * 线程模型：
 *   - UI（LVGL 线程）调用 play/pause/next 等控制 API
 *   - backend 解码任务消费命令并回写快照（进度/频谱/状态）
 *   - 共享数据一律 atomic / 双缓冲快照，无锁
 *
 * v1 范围：WAV(16bit PCM) + MP3；无 seek；FLAC 不做（用户 2026-11-18 拍板）。
 */
#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#define MUSIC_BARS  16                       /* 频谱条数 */
#define MUSIC_TITLE_MAX 96
#define MUSIC_ARTIST_MAX 64
#define MUSIC_PATH_MAX 160
#define MUSIC_LIST_MAX 64

typedef struct {
    char path[MUSIC_PATH_MAX];
    char title[MUSIC_TITLE_MAX];
    char artist[MUSIC_ARTIST_MAX];
    char fmt[8];                /* "MP3" / "WAV" */
    int  duration_s;            /* 总时长（backend open 时填，0=未知） */
} music_track_t;

typedef enum {
    MUSIC_STOPPED = 0,
    MUSIC_PLAYING,
    MUSIC_PAUSED,
} music_state_t;

typedef enum {
    MUSIC_LOOP_ALL = 0,         /* 列表循环（默认） */
    MUSIC_LOOP_ONE,             /* 单曲循环 */
    MUSIC_LOOP_OFF,             /* 顺序播完即停 */
} music_loop_t;

/* ---------------- 控制与状态（UI 调用） ---------------- */

void music_player_init(void);   /* 幂等：注册 backend、清状态 */
void music_player_scan(void);   /* 扫描曲库（阻塞 IO——worker 线程调用！） */
const char *music_player_scan_note(void); /* 最近扫描的空态说明（""=非空态） */
int  music_player_count(void);
const music_track_t *music_player_track(int idx);

/* 播放/暂停/恢复/停止；返回 0=OK（状态已变或已在目标态），-1=无可播 */
int  music_player_play(int idx);        /* -1 或越界 = 当前曲重播 */
void music_player_pause(void);
void music_player_resume(void);
void music_player_stop(void);
void music_player_next(int dir);        /* +1/-1；随机模式下随机选 */
void music_player_toggle(void);         /* 播放↔暂停 */

/* 播放顺序模式 */
void music_player_set_shuffle(bool on);
bool music_player_get_shuffle(void);
void music_player_cycle_loop(void);     /* all → one → off → all */
const char *music_player_loop_text(void); /* "列表循环"/"单曲循环"/"顺序播放" */

/* ---------------- 只读快照（UI timer 每 200ms 拉取） ---------------- */

music_state_t music_player_state(void);
int  music_player_cur_idx(void);
void music_player_get_progress(int *cur_s, int *total_s);
void music_player_get_spectrum(uint8_t bars[MUSIC_BARS]); /* 0..255 */

/* ---------------- 事件（backend 回写） ---------------- */

/* 曲目播完（eos）由 backend 调用：按 loop/shuffle 决定下一首或停 */
void music_player_on_track_end(void);

#endif /* MUSIC_PLAYER_H */

/* WAV/RIFF 头解析（music_player.c 实现，双端 backend 共用） */
int music_riff_parse(FILE *f, int *rate, int *ch, int *bits,
                     long *data_off, long *data_len);
