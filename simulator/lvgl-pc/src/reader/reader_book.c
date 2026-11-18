/* reader_book.c — TXT 阅读器核心层实现（双端）。
 *
 * 大文件策略：全书不进 RAM——16KB 流式读缓冲（PSRAM 惰性）+ 文件 byte
 * offset 分页/章节表。排版宽度用 LVGL 字体度量（lv_text_get_size 单字符）。
 *
 * 线程：所有函数阻塞（fread/整书扫描），仅 worker 线程调用；UI 线程
 * 只消费渲染产物 reader_page_t（worker 渲染 → lv_async_call 回 UI）。
 */
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl.h"
#include "cJSON.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define RLOG(...) ESP_LOGI("reader", __VA_ARGS__)
#else
#define heap_caps_malloc(sz, caps) malloc(sz)
#define heap_caps_free(p) free(p)
#define MALLOC_CAP_SPIRAM 0
#define RLOG(...) do { printf("[reader] "); printf(__VA_ARGS__); \
        printf("\n"); fflush(stdout); } while (0)
#endif
#include "data_source.h"
#include "sd_paths.h"
#include "gbk_table.h"
#include "reader_book.h"

#define RD_CHUNK (16 * 1024)

extern const lv_font_t lv_font_cn_16;
extern const lv_font_t lv_font_cn_20;
extern const lv_font_t lv_font_cn_24;

struct reader_book {
    FILE *f;
    long fsize;
    int enc;                    /* 0=UTF-8 1=GBK */
    char title[RD_TITLE_MAX];
    char path[RD_PATH_MAX];
    reader_chapter_t *chs;      /* 章节表（PSRAM 惰性） */
    int ch_n, ch_cap;
    long *pgs;                  /* 当前章页起点表（PSRAM 惰性） */
    int pg_n, pg_cap, pg_ch;
    /* 流式解码状态 */
    uint8_t *buf;               /* RD_CHUNK PSRAM 惰性 */
    long buf_fpos;
    int buf_len, buf_pos;
    /* 排版参数（render 前由 UI 侧 set） */
    const lv_font_t *font;
    int font_px, gap_px, line_h, line_max_w, page_h;
};

/* ---------------- 流式解码 ---------------- */

/* refill：缓冲空时读下一块；返回可用字节数（0=EOF） */
static int buf_refill(reader_book_t *b)
{
    if (b->buf_pos < b->buf_len) return b->buf_len - b->buf_pos;
    b->buf_fpos += b->buf_len;
    b->buf_len = (int)fread(b->buf, 1, RD_CHUNK, b->f);
    b->buf_pos = 0;
    if (b->buf_len <= 0) { b->buf_len = 0; return 0; }
    return b->buf_len;
}

static int buf_peek(reader_book_t *b)
{
    if (!buf_refill(b)) return -1;
    return b->buf[b->buf_pos];
}

static void buf_next(reader_book_t *b) { b->buf_pos++; }

/* 解码流当前位置 = "下一个未消费字符"的文件 offset */
static long buf_tell(reader_book_t *b)
{
    return b->buf_fpos + b->buf_pos;
}

/* 重置解码流到任意 offset */
static void buf_seek(reader_book_t *b, long off)
{
    b->buf_fpos = off;
    b->buf_len = 0;
    b->buf_pos = 0;
    fseek(b->f, off, SEEK_SET);
}

/* 轻量回退（mark 必须在当前缓冲窗口内——dec_next 一次消费 ≤4B，恒成立） */
static void buf_rewind(reader_book_t *b, long mark)
{
    if (mark >= b->buf_fpos && mark < b->buf_fpos + b->buf_len)
        b->buf_pos = (int)(mark - b->buf_fpos);
    else
        buf_seek(b, mark);
}

/* codepoint → UTF-8 写入 out，返回字节数 */
static int utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* 解下一个字符：UTF-8 文件直取并校验；GBK 文件双字节查表换码。
 * 返回消费的原始字节数（0=EOF）；*cp=codepoint（非法 → 0xFFFD）。
 * GBK 单字节区直通 ASCII；扩展区（表外）降级 '?'。 */
static int dec_next(reader_book_t *b, uint32_t *cp)
{
    int c = buf_peek(b);
    if (c < 0) { *cp = 0; return 0; }

    if (b->enc == 0) {
        int n;
        if (c < 0x80) n = 1;
        else if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        else { buf_next(b); *cp = 0xFFFD; return 1; }
        uint8_t t[4];
        t[0] = (uint8_t)c;      /* 首字节（peek 未消费，直接取用） */
        buf_next(b);
        for (int i = 1; i < n; i++) {
            int cb = buf_peek(b);
            if (cb < 0) { b->buf_pos -= i - 1; *cp = 0xFFFD; return 1; }
            t[i] = (uint8_t)cb;
            buf_next(b);
            if ((t[i] & 0xC0) != 0x80) {
                b->buf_pos -= i - 1; /* 回退续字节，首字节当 0xFFFD */
                *cp = 0xFFFD;
                return 1;
            }
        }
        /* UTF-8 字节 → codepoint（剥前缀位） */
        if (n == 1) *cp = t[0];
        else if (n == 2)
            *cp = ((uint32_t)(t[0] & 0x1F) << 6) | (t[1] & 0x3F);
        else if (n == 3)
            *cp = ((uint32_t)(t[0] & 0x0F) << 12) |
                  ((uint32_t)(t[1] & 0x3F) << 6) | (t[2] & 0x3F);
        else
            *cp = ((uint32_t)(t[0] & 0x07) << 18) |
                  ((uint32_t)(t[1] & 0x3F) << 12) |
                  ((uint32_t)(t[2] & 0x3F) << 6) | (t[3] & 0x3F);
        return n;
    }

    /* GBK */
    if (c < 0x80) { buf_next(b); *cp = (uint32_t)c; return 1; }
    buf_next(b);                /* 消费首字节 */
    int lo = buf_peek(b);
    if (lo < 0) { *cp = '?'; return 1; }
    buf_next(b);                /* 消费次字节 */
    if (c >= 0xA1 && c <= 0xF7 && lo >= 0xA1 && lo <= 0xFE) {
        uint16_t v = gbk2312_cp[(c - 0xA1) * 94 + (lo - 0xA1)];
        *cp = v ? v : '?';
        return 2;
    }
    *cp = '?';
    return 2;
}

/* ---------------- 编码检测 ---------------- */

/* 严格 UTF-8 校验（校验失败 → GBK 兜底） */
static int utf8_valid(const uint8_t *p, int n)
{
    int i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        int need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;
        for (int k = 1; k <= need; k++) {
            if (i + k >= n) break; /* 结尾截断不判失败 */
            if ((p[i + k] & 0xC0) != 0x80) return 0;
        }
        i += need + 1;
    }
    for (int x = 0; x < n; x++)
        if (p[x] == 0) return 0; /* NUL = 二进制 */
    return 1;
}

/* 读首 64KB 判编码：BOM → 严格校验 → GBK。写 b->enc。
 * 缓冲走 PSRAM（worker 栈只有 16KB，坑 #13——64KB 栈缓冲必溢出）。 */
static void detect_encoding(reader_book_t *b, FILE *f)
{
    uint8_t *head = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    if (!head) { b->enc = 0; return; }
    int n = (int)fread(head, 1, 64 * 1024, f);
    if (n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
        b->enc = 0;
    } else {
        b->enc = utf8_valid(head, n) ? 0 : 1;
    }
    heap_caps_free(head);
}

/* ---------------- 章节识别 ---------------- */

/* UTF-8 安全截断（不切半个多字节字符） */
static void utf8_trunc(const char *s, int cap, char *out)
{
    int n = (int)strlen(s);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, s, (size_t)n);
    out[n] = 0;
}

/* 章节行判定（UTF-8 行，已 trim；误判容忍：行长 <100B 才参与）。
 * 模式：第+数字词+[章回卷节篇集部] / Chapter N / 序章楔子尾声等特殊行 */
static int is_chapter_line(const char *s)
{
    int blen = (int)strlen(s);
    if (blen <= 0 || blen >= 100) return 0;

    static const char *sp[] = { "序章", "楔子", "尾声", "终章",
                                "后记", "前言", "番外", "自序" };
    for (size_t i = 0; i < sizeof(sp) / sizeof(sp[0]); i++) {
        if (strncmp(s, sp[i], strlen(sp[i])) == 0 && blen < 60) return 1;
    }

    if (strncmp(s, "第", 3) == 0) {
        int i = 3, got = 0;
        while (i <= blen) {
            if (s[i] >= '0' && s[i] <= '9') { i++; got = 1; continue; }
            if (s[i] == ' ' || s[i] == '\t') { i++; continue; }
            if (i + 3 > blen) break;
            char tmp[4];
            memcpy(tmp, s + i, 3);
            tmp[3] = 0;
            if (strstr("零一二三四五六七八九十百千两〇", tmp)) {
                i += 3;
                got = 1;
                continue;
            }
            break;
        }
        if (!got) return 0;
        if (i + 3 > blen) return 0;
        char u[4];
        memcpy(u, s + i, 3);
        u[3] = 0;
        return strstr("章回卷节篇集部", u) != NULL;
    }

    if (strncasecmp(s, "chapter", 7) == 0) {
        const char *p = s + 7;
        while (*p == ' ' || *p == '\t') p++;
        return *p >= '0' && *p <= '9';
    }
    return 0;
}

/* ---------------- 章节扫描 ---------------- */

/* PSRAM 数组增长（失败保持旧表，上层容错） */
static void xgrow(void **pp, int *cap, int esz, int need)
{
    if (*cap >= need) return;
    int nc = *cap ? *cap * 2 : 16;
    while (nc < need) nc *= 2;
    void *np = heap_caps_malloc(nc * esz, MALLOC_CAP_SPIRAM);
    if (!np) return;
    if (*pp) {
        memcpy(np, *pp, (size_t)*cap * esz);
        heap_caps_free(*pp);
    }
    *pp = np;
    *cap = nc;
}

static void add_chapter(reader_book_t *b, long off, const char *title)
{
    if (b->ch_n >= RD_CH_MAX) return;
    xgrow((void **)&b->chs, &b->ch_cap, sizeof(reader_chapter_t),
          b->ch_n + 1);
    if (b->ch_cap < b->ch_n + 1) return; /* 增长失败：丢章不计 */
    reader_chapter_t *c = &b->chs[b->ch_n++];
    c->off = off;
    utf8_trunc(title, RD_CH_TITLE, c->title);
}

/* 全文章节扫描：流式逐行（复用解码器），命中 is_chapter_line 记录。
 * 首章强制 off=0（占位=书名）；若文件首行本身是章节行则去重。 */
static void scan_chapters(reader_book_t *b)
{
    b->ch_n = 0;
    add_chapter(b, 0, b->title);
    buf_seek(b, 0);

    char line[RD_LINE_MAX];
    int eof = 0;
    while (!eof) {
        long line_off = buf_tell(b);
        int len = 0;
        line[0] = 0;
        int ended = 0;
        while (!ended) {
            uint32_t cp;
            int n = dec_next(b, &cp);
            if (n == 0) { eof = 1; break; }
            if (cp == '\n') { ended = 1; break; }
            if (cp == '\r') continue;
            if (len + 4 > RD_LINE_MAX - 1) continue; /* 超长行只消费不存 */
            len += utf8_put(cp, line + len);
            line[len] = 0;
        }
        if (eof && len == 0) break;
        if (len < RD_LINE_MAX - 1) {
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            int tl = (int)strlen(t);
            while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t'))
                t[--tl] = 0;
            if (is_chapter_line(t)) add_chapter(b, line_off, t);
        }
    }
    /* 首行即真章节行 → 删占位章（chs[0].off==0 与 chs[1].off==0 重复） */
    if (b->ch_n > 1 && b->chs[1].off == 0) {
        memmove(&b->chs[0], &b->chs[1],
                (size_t)(b->ch_n - 1) * sizeof(reader_chapter_t));
        b->ch_n--;
    }
}

/* ---------------- 排版渲染 ---------------- */

/* 排版参数（worker 内 render 前调用） */
static void set_layout(reader_book_t *b, int font_px, int gap_px,
                       int w, int h)
{
    static const lv_font_t *const fonts[] = {
        &lv_font_cn_16, &lv_font_cn_20, &lv_font_cn_24
    };
    int idx = font_px <= 16 ? 0 : (font_px <= 20 ? 1 : 2);
    b->font = fonts[idx];
    b->font_px = font_px;
    b->gap_px = gap_px;
    b->line_h = b->font->line_height + gap_px;
    b->line_max_w = w;
    b->page_h = h;
}

/* 字符显示宽度：CJK 统一按字号宽（Noto SC 方块字 + 全角标点），其余实测 */
static int char_w(reader_book_t *b, uint32_t cp)
{
    if (cp == '\t') cp = ' ';
    if (cp >= 0x2E80 || cp == 0x3000) {
        return b->font_px;
    }
    char s[5];
    int n = utf8_put(cp, s);
    s[n] = 0;
    lv_point_t sz;
    lv_text_get_size(&sz, s, b->font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* 行首禁则（标点不打头） */
static int bad_head(uint32_t cp)
{
    static const char *puncts = "，。、；：？！」』）】〉》…—·‰";
    char s[4];
    int n = utf8_put(cp, s);
    if (n == 3) {
        for (const char *p = puncts; *p; p += 3) {
            if (p[0] == s[0] && p[1] == s[1] && p[2] == s[2]) return 1;
        }
        return 0;
    }
    return n == 1 && cp != 0 && strchr("!?,.:;%)]}>", (char)cp) != NULL;
}

/* 行尾禁则（开括号不悬挂行尾） */
static int bad_tail(uint32_t cp)
{
    static const char *puncts = "（【「『〈《";
    char s[4];
    int n = utf8_put(cp, s);
    if (n == 3) {
        for (const char *p = puncts; p[0]; p += 3) {
            if (p[0] == s[0] && p[1] == s[1] && p[2] == s[2]) return 1;
        }
        return 0;
    }
    return n == 1 && strchr("([{<", (char)cp) != NULL;
}

/* 渲染一页（公共 API，worker 线程调用） */
int reader_render(reader_book_t *b, long off, int ch_hint,
                  int font_px, int gap_px, int w, int h, reader_page_t *out)
{
    if (!b || !b->f) return -1;
    set_layout(b, font_px, gap_px, w, h);
    int cap = b->page_h / b->line_h;
    if (cap < 1) cap = 1;
    if (cap > RD_MAX_LINES) cap = RD_MAX_LINES;

    out->nlines = 0;
    out->lines[0][0] = 0;
    out->page_off = off;
    out->next_off = off;
    out->ch_idx = ch_hint;

    /* 段落首判定：off-1 是否 '\n'（字节级判定，GBK 双字节次字节 ≥0x40
     * 不会误命中；页边界恒为行边界，off 本身是解码流边界） */
    int para = 1;
    if (off > 0) {
        fseek(b->f, off - 1, SEEK_SET);
        para = (fgetc(b->f) == '\n');
    }
    buf_seek(b, off);

    char cur[RD_LINE_MAX];
    int cur_len = 0, line_w = 0;
    int prev_nl = para;         /* 当前行是段落首 → 首字符前缩进 */
    int nlines = 0;
    long next = off;

    for (;;) {
        long mark = buf_tell(b);
        uint32_t cp;
        int n = dec_next(b, &cp);
        if (n == 0) {           /* EOF */
            if (cur_len > 0) {
                memcpy(out->lines[nlines++], cur, (size_t)cur_len + 1);
            }
            next = buf_tell(b);
            break;
        }
        if (nlines >= cap) { next = mark; break; }
        if (cp == '\n') {
            memcpy(out->lines[nlines++], cur, (size_t)cur_len + 1);
            cur_len = 0;
            line_w = 0;
            cur[0] = 0;
            prev_nl = 1;
            if (nlines >= cap) { next = buf_tell(b); break; }
            continue;
        }
        if (cp == '\r') continue;

        if (cur_len == 0 && prev_nl) {
            memcpy(cur, "　　", 7);  /* 两个 U+3000（6B+NUL） */
            cur_len = 6;
            line_w = 2 * b->font_px;
        }
        int cw = char_w(b, cp);
        /* 行尾禁则：开括号放入后若下一字符放不下（lookahead 一字符），
         * 把开括号一起挪到下一行，避免开括悬挂行尾 */
        if (cur_len > 0 && line_w + cw <= b->line_max_w && bad_tail(cp)) {
            long mk2 = buf_tell(b);
            uint32_t nx;
            int nn = dec_next(b, &nx);
            buf_rewind(b, mk2);
            if (nn > 0 && nx != '\n' &&
                line_w + cw + char_w(b, nx) > b->line_max_w) {
                memcpy(out->lines[nlines++], cur, (size_t)cur_len + 1);
                if (nlines >= cap) { next = mark; break; }
                cur_len = 0;
                line_w = 0;
                prev_nl = 0;
            }
        }
        if (line_w + cw <= b->line_max_w || cur_len == 0) {
            /* 放入当前行 */
        } else if (bad_head(cp)) {
            /* 行首禁则：强放入（行超宽容忍，标点不入下一行行首） */
        } else {
            /* 换行：当前行落定 */
            memcpy(out->lines[nlines++], cur, (size_t)cur_len + 1);
            if (nlines >= cap) { next = mark; break; }
            cur_len = 0;
            line_w = 0;
            prev_nl = 0;
        }
        cur_len += utf8_put(cp, cur + cur_len);
        cur[cur_len] = 0;
        line_w += cw;
    }
    out->nlines = nlines;
    out->next_off = next;
    return 0;
}

/* 章节页表：进入新章时全章分页缓存（O(1) 上下页）；
 * 页起点 = 每页首字符 byte offset（与 render 同参数） */
int reader_ch_pages(reader_book_t *b, int ch, int font_px, int gap_px,
                    int w, int h)
{
    if (!b || !b->f) return 0;
    if (b->pg_ch == ch && b->font_px == font_px && b->gap_px == gap_px &&
        b->line_max_w == w && b->page_h == h && b->pg_n > 0)
        return b->pg_n;
    set_layout(b, font_px, gap_px, w, h);
    b->pg_n = 0;
    long end = (ch + 1 < b->ch_n) ? b->chs[ch + 1].off : b->fsize;
    long off = b->chs[ch].off;
    while (off < end) {
        xgrow((void **)&b->pgs, &b->pg_cap, sizeof(long), b->pg_n + 1);
        if (b->pg_cap < b->pg_n + 1) break;
        b->pgs[b->pg_n++] = off;
        reader_page_t pg;
        if (reader_render(b, off, ch, font_px, gap_px, w, h, &pg) != 0)
            break;
        if (pg.next_off <= off) break; /* 末页/异常防死循环 */
        off = pg.next_off;
    }
    b->pg_ch = ch;
    return b->pg_n;
}

/* ---------------- 打开 / 关闭 / 章节访问 ---------------- */

const char *reader_enc_name(reader_book_t *b) { return b->enc ? "GBK" : "UTF-8"; }

int reader_ch_count(reader_book_t *b) { return b ? b->ch_n : 0; }

long reader_fsize(reader_book_t *b) { return b ? b->fsize : 0; }

const char *reader_path(reader_book_t *b) { return b ? b->path : ""; }

long reader_page_offset(reader_book_t *b, int ch, int pg)
{
    if (!b || b->pg_ch != ch || pg < 0 || pg >= b->pg_n) return -1;
    return b->pgs[pg];
}

const char *reader_ch_title(reader_book_t *b, int ch)
{
    if (ch < 0 || ch >= b->ch_n) return "";
    return b->chs[ch].title;
}

long reader_ch_offset(reader_book_t *b, int ch) { return b->chs[ch].off; }

reader_book_t *reader_open(const char *abs_path, const char *rel_path)
{
    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        RLOG("open %s failed", abs_path);
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    reader_book_t *b = heap_caps_malloc(sizeof(struct reader_book),
                                        MALLOC_CAP_SPIRAM);
    if (!b) {
        fclose(f);
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    b->f = f;
    b->fsize = (long)st.st_size;
    b->pg_ch = -1;

    /* 标题 = 文件名去扩展名（UTF-8 安全截断） */
    const char *base = strrchr(abs_path, '/');
    base = base ? base + 1 : abs_path;
    char t[RD_TITLE_MAX];
    snprintf(t, sizeof(t), "%s", base);
    char *dot = strrchr(t, '.');
    if (dot) *dot = 0;
    utf8_trunc(t, RD_TITLE_MAX, b->title);

    detect_encoding(b, f);
    /* 检测读掉了 64KB → 解码流从头开始（buf_seek 重置） */
    b->buf = heap_caps_malloc(RD_CHUNK, MALLOC_CAP_SPIRAM);
    if (!b->buf) {
        reader_close(b);
        return NULL;
    }
    snprintf(b->path, sizeof(b->path), "%s", rel_path);
    scan_chapters(b);
    RLOG("open %s: %ldB enc=%s ch=%d", base, b->fsize,
         b->enc ? "GBK" : "UTF-8", b->ch_n);
    return b;
}

void reader_close(reader_book_t *b)
{
    if (!b) return;
    if (b->f) fclose(b->f);
    heap_caps_free(b->chs);
    heap_caps_free(b->pgs);
    heap_caps_free(b->buf);
    heap_caps_free(b);
}

/* ---------------- 书架扫描 ---------------- */

/* 需要 dirent.h —— 头部 include 未含，这里补（POSIX 目录遍历） */
#include <dirent.h>

static int shelf_cmp(const void *a, const void *b)
{
    const reader_shelf_t *x = a, *y = b;
    if (x->ts != y->ts) return y->ts - x->ts > 0 ? 1 : -1;
    return strcasecmp(x->title, y->title);
}

/* 递归收 .txt（深度 3）；abs_dir 拼文件系统路径，rel_dir 拼进度 key */
static void shelf_walk(const char *abs_dir, const char *rel_dir, int depth,
                       reader_shelf_t *out, int max, int *n)
{
    DIR *d = opendir(abs_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && *n < max) {
        if (de->d_name[0] == '.') continue;
        char abs[RD_PATH_MAX], rel[RD_PATH_MAX];
        snprintf(abs, sizeof(abs), "%s/%s", abs_dir, de->d_name);
        snprintf(rel, sizeof(rel), "%s/%s", rel_dir, de->d_name);
        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) shelf_walk(abs, rel, depth - 1, out, max, n);
            continue;
        }
        size_t L = strlen(de->d_name);
        if (L <= 4 || strcasecmp(de->d_name + L - 4, ".txt") != 0) continue;
        if (*n >= max) break;
        reader_shelf_t *e = &out[(*n)++];
        snprintf(e->path, sizeof(e->path), "%s", rel);
        char t[RD_TITLE_MAX];
        snprintf(t, sizeof(t), "%s", de->d_name);
        char *dot = strrchr(t, '.');
        if (dot) *dot = 0;
        utf8_trunc(t, RD_TITLE_MAX, e->title);
        e->size = (long)st.st_size;
        long off = 0;
        int ch = 0, pg = 0;
        reader_progress_get(e->path, &off, &ch, &pg, &e->ts);
        e->percent = e->size > 0 ? (int)(off * 100 / e->size) : 0;
        e->ch = ch;
    }
    closedir(d);
}

int reader_scan_shelf(reader_shelf_t *out, int max, int *n)
{
    *n = 0;
    char abs[RD_PATH_MAX];
    /* book/ 递归（书库目录与文件管理器根目录分离） */
    if (sd_resolve(abs, sizeof(abs), "book") == 0) {
        shelf_walk(abs, "book", 3, out, max, n);
    }
    qsort(out, (size_t)*n, sizeof(out[0]), shelf_cmp);
    return 0;
}

/* ---------------- 进度 / 配置持久化 ---------------- */

/* data/reader/ 下 JSON 文件路径（目录幂等创建） */
static void reader_json_path(char *out, int cap, const char *name)
{
    char dir[RD_PATH_MAX];
    if (sd_resolve(dir, sizeof(dir), "data/reader") == 0) mkdir(dir, 0777);
    /* 手拼（cap 是参数，GCC -Werror=format-truncation 无法证明安全） */
    size_t dl = strlen(dir);
    if (dl >= (size_t)cap - 1) dl = (size_t)cap - 1;
    memcpy(out, dir, dl);
    size_t nl = strlen(name);
    if (dl + 1 + nl >= (size_t)cap) nl = (size_t)cap - dl - 2;
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl);
    out[dl + 1 + nl] = 0;
}

/* JSON 文件装载（≤8KB）；失败返回 NULL */
static cJSON *load_json_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(8192);
    if (!buf) { fclose(f); return NULL; }
    int n = (int)fread(buf, 1, 8191, f);
    fclose(f);
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

int reader_progress_get(const char *rel, long *off, int *ch, int *pg,
                        long *ts)
{
    if (off) *off = 0;
    if (ch) *ch = 0;
    if (pg) *pg = 0;
    if (ts) *ts = 0;
    char p[RD_PATH_MAX];
    reader_json_path(p, sizeof(p), "progress.json");
    cJSON *root = load_json_file(p);
    if (!root) return -1;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "books")) {
        const cJSON *pp = cJSON_GetObjectItem(it, "path");
        if (!cJSON_IsString(pp) || strcmp(pp->valuestring, rel)) continue;
        const cJSON *j;
        if ((j = cJSON_GetObjectItem(it, "off")) && cJSON_IsNumber(j))
            if (off) *off = (long)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "ch")) && cJSON_IsNumber(j))
            if (ch) *ch = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "pg")) && cJSON_IsNumber(j))
            if (pg) *pg = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(it, "ts")) && cJSON_IsNumber(j))
            if (ts) *ts = (long)j->valuedouble;
        cJSON_Delete(root);
        return 0;
    }
    cJSON_Delete(root);
    return -1;
}

int reader_progress_set(const char *rel, long off, int ch, int pg)
{
    char p[RD_PATH_MAX];
    reader_json_path(p, sizeof(p), "progress.json");
    cJSON *root = load_json_file(p);
    if (!root) root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON *books = cJSON_GetObjectItem(root, "books");
    if (!cJSON_IsArray(books)) {
        books = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "books", books);
    }
    cJSON *it = NULL, *item = NULL;
    cJSON_ArrayForEach(it, books) {
        const cJSON *pp = cJSON_GetObjectItem(it, "path");
        if (cJSON_IsString(pp) && !strcmp(pp->valuestring, rel)) {
            item = it;
            break;
        }
    }
    if (!item) {
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "path", rel);
        cJSON_AddItemToArray(books, item);
    }
    cJSON_ReplaceItemInObject(item, "off", cJSON_CreateNumber((double)off));
    cJSON_ReplaceItemInObject(item, "ch", cJSON_CreateNumber(ch));
    cJSON_ReplaceItemInObject(item, "pg", cJSON_CreateNumber(pg));
    cJSON_ReplaceItemInObject(item, "ts", cJSON_CreateNumber(time(NULL)));
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return -1;
    char tmp[RD_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
        rename(tmp, p);
    }
    cJSON_free(s);
    return 0;
}

/* ---------------- 配置持久化 ---------------- */

void reader_config_load(reader_config_t *cfg)
{
    cfg->font_px = 20;
    cfg->gap_class = 1;
    cfg->theme = 0;
    cfg->auto_sec = 0;
    char p[RD_PATH_MAX];
    reader_json_path(p, sizeof(p), "config.json");
    cJSON *root = load_json_file(p);
    if (!root) return;
    const cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "font")) && cJSON_IsNumber(j))
        cfg->font_px = (int)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "gap")) && cJSON_IsNumber(j))
        cfg->gap_class = (int)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "theme")) && cJSON_IsNumber(j))
        cfg->theme = (int)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "auto")) && cJSON_IsNumber(j))
        cfg->auto_sec = (int)j->valuedouble;
    cJSON_Delete(root);
    if (cfg->font_px != 16 && cfg->font_px != 20 && cfg->font_px != 24)
        cfg->font_px = 20;
    if (cfg->gap_class < 0 || cfg->gap_class > 2) cfg->gap_class = 1;
    if (cfg->theme < 0 || cfg->theme > 2) cfg->theme = 0;
}

void reader_config_save(const reader_config_t *cfg)
{
    char p[RD_PATH_MAX];
    reader_json_path(p, sizeof(p), "config.json");
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "font", cfg->font_px);
    cJSON_AddNumberToObject(root, "gap", cfg->gap_class);
    cJSON_AddNumberToObject(root, "theme", cfg->theme);
    cJSON_AddNumberToObject(root, "auto", cfg->auto_sec);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return;
    char tmp[RD_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
        rename(tmp, p);
    }
    cJSON_free(s);
}

int reader_gap_px(int gap_class, int font_px)
{
    (void)font_px;
    static const int g[] = { 4, 10, 16 };
    if (gap_class < 0) gap_class = 1;
    if (gap_class > 2) gap_class = 2;
    return g[gap_class];
}
