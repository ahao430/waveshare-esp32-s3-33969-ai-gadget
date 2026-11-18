/* reader_book — TXT 小说阅读器核心层（双端：固件/模拟器，ADR-001）。
 *
 * 职责：
 *   - 书架扫描（book/，递归深度 3，只列 .txt）
 *   - 打开书：编码检测（UTF-8 BOM/严格校验 → GB2312 换码表）+ 章节扫描
 *   - 分页排版：按字体度量逐行填充（行首禁则标点/首行缩进），页起点
 *     byte offset 表缓存（当前章），上一页 O(1) 回退
 *   - 断点续读：进度按 byte offset 持久化（data/reader/progress.json，
 *     temp+rename 原子写），书架显示百分比
 *
 * 线程模型（同 ui_files/music 曲库）：
 *   - 全部 IO/解码/排版为阻塞操作——只能在 worker 线程调用
 *   - UI 线程只读 worker 渲染好的 reader_page_t（lv_async_call 回去）
 *
 * 大文件：全书不进 RAM。流式 16KB 读缓冲（PSRAM 惰性）+ 文件偏移分页；
 * 章节表 = 每章起始 offset（PSRAM 数组），无章节 TXT 也支持（单"全文"章）。
 *
 * v1 范围：TXT only；UTF-8/GBK；UTF-16 不支持（提示转存）。
 */
#ifndef READER_BOOK_H
#define READER_BOOK_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define RD_PATH_MAX   300
#define RD_TITLE_MAX  96
#define RD_LIST_MAX   64          /* 书架上限 */
#define RD_CH_MAX     512         /* 章节上限 */
#define RD_CH_TITLE   64          /* 章节标题字节上限（UTF-8 截断） */

/* 一页渲染产物：UI 直接按行建 label */
#define RD_MAX_LINES  16
#define RD_LINE_MAX   200         /* 单行字节上限（424px 版心 / 16px 字号） */
typedef struct {
    char lines[RD_MAX_LINES][RD_LINE_MAX];
    int  nlines;
    long page_off;              /* 本页起始 byte offset */
    long next_off;              /* 下一页起始（= 渲染停止处） */
    int  ch_idx;                /* 渲染所在章 */
} reader_page_t;

typedef struct {
    char path[RD_PATH_MAX];     /* sd 相对路径（进度 key） */
    char title[RD_TITLE_MAX];
    long size;
    long ts;                    /* 最近阅读时间（unix s，0=未读） */
    int  percent;               /* 阅读进度（progress.json），无记录=0 */
    int  ch;                    /* 上次读到章 */
} reader_shelf_t;

typedef struct {
    long off;                   /* 章起始 byte offset */
    char title[RD_CH_TITLE];
} reader_chapter_t;

typedef struct reader_book reader_book_t;

/* ---------------- 书架（worker 调用） ---------------- */

/* 扫 book/ 下 .txt（递归深度 3，不含 SD 根目录），按最近阅读时间排序；
 * 返回条数（*n），无卡/无目录返回 0。 */
int  reader_scan_shelf(reader_shelf_t *out, int max, int *n);

/* 取消在途书架扫描（退出页面时调用）：扫描在文件之间检查标志，快速
 * 返回，避免 UI 已释放缓冲后 worker 仍写入。 */
void reader_scan_cancel(void);

/* 释放 progress.json 解析缓存（退出 App 时调；书架扫描在途时勿调） */
void reader_progress_cache_clear(void);

/* ---------------- 打开/关闭（worker 调用） ---------------- */

/* 打开 + 编码检测 + 全文章节扫描（大文件秒级：一次流式全文扫描）。
 * 返回 NULL = 打不开/二进制/UTF-16。 */
reader_book_t *reader_open(const char *abs_path, const char *rel_path);
void reader_close(reader_book_t *b);

const char *reader_enc_name(reader_book_t *b); /* "UTF-8"/"GBK" */
long reader_fsize(reader_book_t *b);
const char *reader_path(reader_book_t *b);     /* sd 相对路径（进度 key） */
/* 当前章页表中第 pg 页起点（未建/越界返回 -1） */
long reader_page_offset(reader_book_t *b, int ch, int pg);

/* ---------------- 章节与页（worker 调用） ---------------- */

int  reader_ch_count(reader_book_t *b);
const char *reader_ch_title(reader_book_t *b, int ch);
/* 章起始 offset（书架 percent 计算用） */
long reader_ch_offset(reader_book_t *b, int ch);

/* 渲染一页：从 off 起，按 font_px（16/20/24）与版心 w×h 排版。
 * 失败返回 -1（IO 错误）；到文件尾页 next_off == page_off。 */
int  reader_render(reader_book_t *b, long off, int ch_hint,
                   int font_px, int gap_px, int w, int h,
                   reader_page_t *out);

/* 章节页表（章内 O(1) 上下页）：进入新章时重建，返回页数 */
int  reader_ch_pages(reader_book_t *b, int ch, int font_px, int gap_px,
                     int w, int h);

/* ---------------- 进度持久化（worker 调用） ---------------- */

/* 读单本进度：命中返回 0；*ts 可为 NULL（最近阅读排序用） */
int  reader_progress_get(const char *rel_path, long *off, int *ch, int *pg,
                         long *ts);
/* 写进度（整个表重写，temp+rename 原子替换） */
int  reader_progress_set(const char *rel_path, long off, int ch, int pg);

/* ---------------- 设置持久化（worker 调用） ---------------- */

typedef struct {
    int  font_px;               /* 16/20/24 */
    int  gap_class;             /* 0=紧凑 1=标准 2=宽松 */
    int  theme;                 /* 0=霓虹深 1=纯黑 2=护眼暗琥珀 */
    int  auto_sec;              /* 自动翻页秒数；0=关 */
} reader_config_t;

void reader_config_load(reader_config_t *cfg);   /* 默认值兜底 */
void reader_config_save(const reader_config_t *cfg);

/* 行距档位 → 像素（font_px + gap） */
int  reader_gap_px(int gap_class, int font_px);

#endif /* READER_BOOK_H */
