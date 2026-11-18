/* photo_decode - 三格式解码核心（双端，ADR-018）。
 *
 * JPEG: vendor TJpgDec R0.03（src/photo/tjpgd/，JD_USE_SCALE=1），
 *   按"最大边解码后 >=480"选 1/1..1/8 缩放档；12MP 1/8 解码 ~0.2s。
 * PNG: lvgl 内置 lodepng（LV_USE_LODEPNG），整幅 RGBA8 中转，
 *   像素上限 PH_PNG_MAXPX（150K，中转 <=640KB）；超限明确失败（提示转 JPG）。
 * BMP: 自研 24/32bpp 非压缩行流解析（bottom-up/Top-down 都支持）。
 *
 * 统一输出管线：解码行（RGB888/RGBA8）按栅格序推进"行窗写器"，
 *   fit（等比 k 盒式降采样，输出字节 <=720KB）或 cover（中心方裁切
 *   + 有理映射盒式降采样/最近邻上采样到 dst_px^2）。EXIF 旋转不在此层
 *   （流式无法转置），UI 层 lv_image_set_rotation 处理。
 *
 * 内存账（坑 #26/#27 口径，全 PSRAM 惰性）：
 *   全屏 canvas <=720KB + 行窗 <=90KB + TJpgDec workbuf 4KB；
 *   PNG 中转与 canvas 短暂并存 ~900KB；缩略图 LRU 12 槽 240KB 常驻。
 *   App on_exit 调 photo_cache_clear 全释放。
 */
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "photo.h"
#include "sd_paths.h"
#include "tjpgd/tjpgd.h"

/* LVGL 内置 lodepng（LV_USE_LODEPNG=y 时编进 lvgl 库）。其头文件不在
 * 两个构建体系的公开 include 路径上，这里直接声明用到的原型
 * （签名与 lvgl/src/libs/lodepng/lodepng.h 一致；实现走 lv_malloc）。 */
extern unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                 const unsigned char *in, size_t insize);

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define PHMALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#define PHFREE(p) do { if (p) heap_caps_free(p); } while (0)
#else
#define PHMALLOC(size) malloc(size)
#define PHFREE(p) do { if (p) free(p); } while (0)
#endif

/* ---------------- 观测计数（调试日志用） ---------------- */

static int s_thumb_hit, s_thumb_miss, s_full_n, s_fail_n;

void photo_stats_get(int *th, int *tm, int *fu, int *fa)
{
    if (th) *th = s_thumb_hit;
    if (tm) *tm = s_thumb_miss;
    if (fu) *fu = s_full_n;
    if (fa) *fa = s_fail_n;
}

void photo_stats_reset(void)
{
    s_thumb_hit = s_thumb_miss = s_full_n = s_fail_n = 0;
}

/* ---------------- 缩略图 LRU 缓存（12 槽 x 100^2 RGB565） ---------- */

#define PH_THUMB_SLOTS 12
/* 缓存槽表惰性分配（坑 #19：2.7KB .bss 会挤 Wi-Fi RX 底线，
 * 2026-11-18 图片 App 版无限重启实证） */
struct ph_thslot {
    char path[PH_PATH_MAX];
    uint16_t *buf;              /* PH_THUMB_PX^2 RGB565 */
    long stamp;
};
static struct ph_thslot *s_thcache;
static long s_thstamp;

/* 槽表按需建（PSRAM；失败返回 NULL=缓存停用，直通解码） */
static struct ph_thslot *thcache_tbl(void)
{
    if (!s_thcache) {
        s_thcache = PHMALLOC(sizeof(struct ph_thslot) * PH_THUMB_SLOTS);
        if (s_thcache)
            memset(s_thcache, 0,
                   sizeof(struct ph_thslot) * PH_THUMB_SLOTS);
    }
    return s_thcache;
}

/* 命中返回槽位，未命中返回 -1 */
static int thcache_find(const char *abs)
{
    if (!s_thcache) return -1;
    for (int i = 0; i < PH_THUMB_SLOTS; i++)
        if (s_thcache[i].buf && strcmp(s_thcache[i].path, abs) == 0)
            return i;
    return -1;
}

/* 取 LRU 槽位（优先空闲，其次最旧）；返回时 buf 已备好 */
static uint16_t *thcache_slot(const char *abs)
{
    if (!thcache_tbl()) return NULL;
    int slot = -1;
    for (int i = 0; i < PH_THUMB_SLOTS; i++)
        if (!s_thcache[i].buf) { slot = i; break; }
    if (slot < 0) {
        long oldest = -1;
        for (int i = 0; i < PH_THUMB_SLOTS; i++)
            if (oldest < 0 || s_thcache[i].stamp < oldest) {
                oldest = s_thcache[i].stamp;
                slot = i;
            }
        if (slot >= 0) PHFREE(s_thcache[slot].buf);
        s_thcache[slot].buf = NULL;
    }
    snprintf(s_thcache[slot].path, sizeof(s_thcache[slot].path), "%s", abs);
    s_thcache[slot].stamp = ++s_thstamp;
    s_thcache[slot].buf = PHMALLOC((size_t)PH_THUMB_PX * PH_THUMB_PX * 2);
    return s_thcache[slot].buf;
}

void photo_cache_clear(void)
{
    if (!s_thcache) return;
    for (int i = 0; i < PH_THUMB_SLOTS; i++)
        if (s_thcache[i].buf) { PHFREE(s_thcache[i].buf); s_thcache[i].buf = NULL; }
    PHFREE(s_thcache);
    s_thcache = NULL;
}

/* ---------------- 画布 -> RGB565 转换 ---------------- */

#define PH_CANVAS_BYTES (720L * 1024)   /* 解码画布字节上限（PSRAM 账内） */

#define RGB565(r, g, b) \
    (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

/* 解码画布（RGB888 bpp=3 / RGBA8 bpp=4）-> RGB565 目标。
 * cover=1：中心方窗映射到 dp²（盒式降采样/最近邻上采样）；
 * cover=0：整图等比 k 盒式，输出字节 <=PH_CANVAS_BYTES。 */
static void conv_to_dst(const uint8_t *src, int bpp, int sw, int sh,
                        uint16_t *dst, int dp, int cover,
                        int *out_w, int *out_h)
{
    int cx = 0, cy = 0, cw = sw, ch = sh;
    int ow, oh;
    if (cover) {
        cw = (sw < sh) ? sw : sh;
        ch = cw;
        cx = (sw - cw) / 2;
        cy = (sh - cw) / 2;
        ow = dp;
        oh = dp;
    } else {
        /* fit：等比缩到每边 ≤dp（不放大），有理映射盒式降采样。
         * ow/oh 都不超 dp，调用方 dp² 缓冲恒够（不溢出 dst）；
         * 画布字节上限在解码层已兜底。 */
        if (sw > dp || sh > dp) {
            if (sw >= sh) {
                ow = dp;
                oh = (int)((long)sh * dp / sw);
            } else {
                oh = dp;
                ow = (int)((long)sw * dp / sh);
            }
            if (ow < 1) ow = 1;
            if (oh < 1) oh = 1;
        } else {
            ow = sw;
            oh = sh;
        }
        cw = sw;
        ch = sh;
    }
    *out_w = ow;
    *out_h = oh;
    for (int y = 0; y < oh; y++) {
        int y0 = cy + (int)((long)y * ch / oh);
        int y1 = cy + (int)((long)(y + 1) * ch / oh);
        if (y1 == y0) y1 = y0 + 1;
        if (y1 > cy + ch) y1 = cy + ch;
        for (int x = 0; x < ow; x++) {
            int x0 = cx + (int)((long)x * cw / ow);
            int x1 = cx + (int)((long)(x + 1) * cw / ow);
            if (x1 == x0) x1 = x0 + 1;
            if (x1 > cx + cw) x1 = cx + cw;
            unsigned sr = 0, sg = 0, sb = 0;
            for (int r = y0; r < y1; r++) {
                const uint8_t *rp = src + ((long)r * sw + x0) * bpp;
                for (int c = x0; c < x1; c++) {
                    if (bpp == 2) {
                        uint16_t v;
                        memcpy(&v, rp, 2);
                        sr += (v >> 8) & 0xF8;
                        sg += (v >> 3) & 0xFC;
                        sb += (v << 3) & 0xF8;
                    } else {
                        sr += rp[0];
                        sg += rp[1];
                        sb += rp[2];
                    }
                    rp += bpp;
                }
            }
            int n = (y1 - y0) * (x1 - x0);
            dst[(long)y * ow + x] =
                RGB565(sr / n, sg / n, sb / n);
        }
    }
}

/* ---------------- JPEG（vendor TJpgDec 直调） ---------------- */

/* TJpgDec workbuf（JD_FASTDECODE=1 规格值，多给一点余量） */
#define JPEG_WORKBUF 4096

typedef struct {
    FILE *f;                    /* 输入流 */
    uint8_t *canvas;            /* RGB888 解码画布 */
    int cw, ch;                 /* 画布尺寸（缩放解码后） */
} jpeg_ctx_t;

/* in_func：读 n 字节进 buff；buff=NULL = 跳过 n 字节 */
static size_t jpeg_in(JDEC *jd, uint8_t *buff, size_t n)
{
    FILE *f = ((jpeg_ctx_t *)jd->device)->f;
    if (buff) return fread(buff, 1, n, f);
    return fseek(f, (long)n, SEEK_CUR) == 0 ? n : 0;
}

/* out_func：解码 rect 按行拷进画布。TJpgDec 的 RGB888 字节序是
 * B,G,R（源码 pix 组装注释即 B/G/R），这里换序成 R,G,B，
 * 与 PNG 画布（RGBA8）的红蓝口径统一 */
static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int rw = rect->right - rect->left + 1;
    for (int r = rect->top; r <= rect->bottom; r++) {
        uint8_t *dst = c->canvas
                       + ((size_t)r * c->cw + rect->left) * 3;
        for (int x = 0; x < rw; x++) {
            dst[x * 3] = src[x * 3 + 2];        /* R */
            dst[x * 3 + 1] = src[x * 3 + 1];    /* G */
            dst[x * 3 + 2] = src[x * 3];        /* B */
        }
        src += (size_t)rw * 3;
    }
    return 1;
}

/* 缩放档选择：use_min=1 按"两边都 >=floor"选（缩略图 cover 需要），
 * use_min=0 按"最大边 >=floor"选（全屏 fit 质量下限）。 */
static int jpeg_scale_pick(int w, int h, int floor_px, int use_min)
{
    int s = 0;
    for (int t = 0; t <= 3; t++) {
        int dw = (w + (1 << t) - 1) >> t;
        int dh = (h + (1 << t) - 1) >> t;
        int ok = use_min ? (dw >= floor_px && dh >= floor_px)
                         : (dw >= floor_px || dh >= floor_px);
        if (ok) s = t;
    }
    return s;
}

/* 缩放解码到 RGB888 画布（档位自动选 + 字节上限逐档放宽）。
 * 0=OK / -1=失败；画布由调用方 PHFREE。 */
static int jpeg_decode_scale(const char *abs, int floor_px, int use_min,
                             uint8_t **out, int *out_w, int *out_h)
{
    JDEC jdec;
    JRESULT rc = JDR_PAR;
    uint8_t *work = PHMALLOC(JPEG_WORKBUF);
    jpeg_ctx_t ctx = { 0 };
    int ret = -1;
    FILE *f = fopen(abs, "rb");
    if (!f || !work) goto done;
    ctx.f = f;
    rc = jd_prepare(&jdec, jpeg_in, work, JPEG_WORKBUF, &ctx);
    if (rc != JDR_OK) goto done;
    int s = jpeg_scale_pick(jdec.width, jdec.height, floor_px, use_min);
    for (; s <= 3; s++) {
        int dw = (jdec.width + (1 << s) - 1) >> s;
        int dh = (jdec.height + (1 << s) - 1) >> s;
        if ((size_t)dw * dh * 3 > PH_CANVAS_BYTES) continue;
        ctx.canvas = PHMALLOC((size_t)dw * dh * 3);
        if (!ctx.canvas) break;
        ctx.cw = dw;
        ctx.ch = dh;
        JRESULT rc2 = jd_decomp(&jdec, jpeg_out, (uint8_t)s);
        if (rc2 == JDR_OK || rc2 == JDR_INTR) {
            *out = ctx.canvas;
            *out_w = dw;
            *out_h = dh;
            ret = 0;
        }
        break;
    }
done:
    if (ret != 0 && ctx.canvas) PHFREE(ctx.canvas);
    if (work) PHFREE(work);
    if (f) fclose(f);
    return ret;
}

/* ---------------- PNG（lvgl 内置 lodepng；输出用 lv_free 释放） ------ */

/* PNG 整文件字节上限：≤160K 像素的合法 PNG 压缩后必然小于原始
 * RGBA 尺寸（≤640KB），1MB 已有裕量；超限直接拒绝（不做大块
 * 分配，固件 PSRAM 坑 #22 账内）。 */
#define PH_PNG_FILE_MAX (1024L * 1024)

/* PNG 解码 RGBA8（像素上限内）。0=OK / -1=失败（超限/损坏）。
 * 走内存接口 lodepng_decode32：lvgl 内置版的 *_file 变体经
 * lv_fs（需要盘符前缀，双端不可移植），文件由本模块自读。
 * 注意：LVGL 9 的 lodepng 返回 lv_draw_buf_t（ARGB8888 格式，
 * 小端内存字节序恰为 R,G,B,A，stride=4*w 连续）——这里拷成
 * 常规 PHMALLOC 画布并即毁 draw_buf，三格式画布所有权统一
 * （调用方 PHFREE）；≤160K 像素时双缓冲瞬态 ≤2×640KB。 */
static int png_to_canvas(const char *abs, uint8_t **out, int *out_w,
                         int *out_h)
{
    FILE *f = fopen(abs, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 8 || sz > PH_PNG_FILE_MAX) {
        fclose(f);
        return -1;
    }
    uint8_t *data = PHMALLOC((size_t)sz);
    if (!data) {
        fclose(f);
        return -1;
    }
    size_t n = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        PHFREE(data);
        return -1;
    }
    unsigned char *bufp = NULL;
    unsigned w, h;
    unsigned err = lodepng_decode32(&bufp, &w, &h, data, (size_t)sz);
    PHFREE(data);
    if (err) return -1;
    if ((long)w * h > PH_PNG_MAXPX) {
        lv_draw_buf_destroy((lv_draw_buf_t *)bufp);
        return -1;
    }
    lv_draw_buf_t *db = (lv_draw_buf_t *)bufp;
    uint8_t *cv = PHMALLOC((size_t)w * h * 4);
    if (!cv) {
        lv_draw_buf_destroy(db);
        return -1;
    }
    memcpy(cv, db->data, (size_t)w * h * 4);
    lv_draw_buf_destroy(db);
    *out = cv;
    *out_w = (int)w;
    *out_h = (int)h;
    return 0;
}

/* ---------------- BMP（24/32bpp 非压缩，行流采样） ---------------- */

typedef struct {
    int w, h, bpp;              /* h 已取正 */
    int topdown;                /* 原 h 为负 = top-down 存储 */
    long data_off;              /* 像素数据偏移 */
    long stride;                /* 行跨距（BMP 行 4 字节对齐） */
} bmp_hdr_t;

/* LE 读数小工具 */
static unsigned bmp_rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

/* 解析 BMP 头。0=OK / -1=不支持（非 24/32bpp 或压缩格式）。 */
static int bmp_parse(FILE *f, bmp_hdr_t *b)
{
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) return -1;
    if (hdr[0] != 'B' || hdr[1] != 'M') return -1;
    b->bpp = hdr[28] | hdr[29] << 8;
    if (b->bpp != 24 && b->bpp != 32) return -1;
    if (hdr[30] | hdr[31] | hdr[32] | hdr[33]) return -1;
    b->w = (int)bmp_rd32(hdr + 18);
    b->h = (int)bmp_rd32(hdr + 22);
    b->data_off = (long)bmp_rd32(hdr + 10);
    b->topdown = b->h < 0;
    if (b->h < 0) b->h = -b->h;
    b->stride = ((long)b->w * (b->bpp / 8) + 3) & ~3L;
    return 0;
}

/* BMP 行流采样：单画布行累加器 -> RGB565 行落盘（内存恒 ~cw*12B，
 * 坑 #22/#26 账内；源行序相邻必映射到相邻画布行，行切换即整行完成） */
static void bmp_row_flush(uint8_t *cv, const uint32_t *acc, int cr,
                          int rows, int cw, int bw, int k)
{
    uint16_t *orow = (uint16_t *)(cv + (size_t)cr * cw * 2);
    for (int cc = 0; cc < cw; cc++) {
        int c0 = cc * k;
        int c1 = c0 + k < bw ? c0 + k : bw;
        int n = rows * (c1 - c0);
        if (n <= 0) n = 1;
        orow[cc] = RGB565(acc[cc * 3] / n,
                          acc[cc * 3 + 1] / n,
                          acc[cc * 3 + 2] / n);
    }
}

/* BMP 全图行流 k 盒式采样 -> RGB565 画布（字节上限内）。0=OK/-1。 */
static int bmp_to_canvas(const char *abs, uint8_t **out, int *ow, int *oh)
{
    FILE *f = fopen(abs, "rb");
    if (!f) return -1;
    bmp_hdr_t b;
    int ret = -1;
    uint8_t *cv = NULL;
    uint32_t *acc = NULL;
    int cur_cr = -1, cur_rows = 0;
    uint8_t *row = NULL;
    if (bmp_parse(f, &b) == 0
        && b.w > 0 && b.h > 0 && b.w <= 16384 && b.h <= 16384) {
        int k = 1;
        while (k < 64
               && (long)((b.w + k - 1) / k) * ((b.h + k - 1) / k) * 2
                   > PH_CANVAS_BYTES)
            k++;
        int cw = (b.w + k - 1) / k;
        int ch = (b.h + k - 1) / k;
        row = malloc((size_t)b.w * 4);
        cv = PHMALLOC((size_t)cw * ch * 2);
        acc = PHMALLOC((size_t)cw * 3 * sizeof(uint32_t));
        if (!row || !cv || !acc) goto done;
        memset(cv, 0, (size_t)cw * ch * 2);
        int bstart = b.topdown ? 0 : b.h - 1;
        int bstep = b.topdown ? 1 : -1;
        for (int j = 0; j < b.h; j++) {
            int stored = bstart + j * bstep;   /* 文件行序（bottom-up 兼容） */
            int cr = j / k;                    /* 画布行按视觉行序（j 即视觉 y） */
            if (cr != cur_cr) {                /* 视觉行序相邻，画布行整行切换 */
                if (cur_cr >= 0)
                    bmp_row_flush(cv, acc, cur_cr, cur_rows, cw, b.w, k);
                cur_cr = cr;
                cur_rows = 0;
                memset(acc, 0, (size_t)cw * 3 * 4);
            }
            if (fseek(f, b.data_off + (long)stored * b.stride,
                      SEEK_SET) != 0) goto done;
            if (fread(row, 1, (size_t)b.w * (b.bpp / 8), f)
                != (size_t)b.w * (b.bpp / 8)) goto done;
            const uint8_t *p = row;
            for (int cc = 0; cc < cw; cc++) {
                int c0 = cc * k;
                int c1 = c0 + k < b.w ? c0 + k : b.w;
                unsigned sr = 0, sg = 0, sb = 0;
                for (int c = c0; c < c1; c++) {
                    sr += p[2];                /* BMP 像素序 BGR */
                    sg += p[1];
                    sb += p[0];
                    p += (b.bpp / 8);
                }
                acc[cc * 3] += sr;
                acc[cc * 3 + 1] += sg;
                acc[cc * 3 + 2] += sb;
            }
            cur_rows++;
        }
        if (cur_cr >= 0)
            bmp_row_flush(cv, acc, cur_cr, cur_rows, cw, b.w, k);
        *out = cv;
        *ow = cw;
        *oh = ch;
        cv = NULL;
        ret = 0;
    }
done:
    if (cv) PHFREE(cv);
    if (acc) PHFREE(acc);
    if (row) free(row);
    if (f) fclose(f);
    return ret;
}

/* ---------------- 三格式统一解码到画布 ---------------- */

/* 按格式分派解码到画布。bpp：3=RGB888 / 4=RGBA8 单像素字节数。
 * 0=OK / -1=失败；画布一律 PHMALLOC，由调用方 PHFREE。 */
static int decode_to_canvas(const char *abs, photo_fmt_t fmt,
                            int floor_px, int use_min,
                            uint8_t **cv, int *cw, int *ch, int *bpp)
{
    if (fmt == PH_FMT_JPEG) {
        *bpp = 3;
        return jpeg_decode_scale(abs, floor_px, use_min, cv, cw, ch);
    }
    if (fmt == PH_FMT_PNG) {
        *bpp = 4;
        return png_to_canvas(abs, cv, cw, ch);
    }
    if (fmt == PH_FMT_BMP) {
        *bpp = 2;
        return bmp_to_canvas(abs, cv, cw, ch);
    }
    return -1;
}

/* ---------------- JPEG EXIF 方向（APP1/TIFF IFD0 tag 0x0112） -------- */

/* 字节序读数小工具（EXIF TIFF：II=LE / MM=BE） */
static unsigned exif_rd16(const uint8_t *p, int le)
{
    return le ? (unsigned)p[0] | ((unsigned)p[1] << 8)
              : ((unsigned)p[0] << 8) | p[1];
}

static unsigned exif_rd32(const uint8_t *p, int le)
{
    return le ? (unsigned)p[0] | ((unsigned)p[1] << 8)
              | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24)
              : ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
              | ((unsigned)p[2] << 8) | p[3];
}

/* TIFF 块（II/MM + 42 + IFD0 偏移）内找 Orientation（SHORT，内联值） */
static int exif_tiff_orientation(const uint8_t *t, size_t n)
{
    if (n < 8) return 0;
    int le = (t[0] == 'I' && t[1] == 'I');
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 0;
    unsigned ifd = exif_rd32(t + 4, le);
    if (ifd < 8 || ifd + 2 > n) return 0;
    const uint8_t *e = t + ifd;
    unsigned cnt = exif_rd16(e, le);
    if (ifd + 2 + cnt * 12 > n) cnt = (n - ifd - 2) / 12;   /* 窗口内截断 */
    for (unsigned k = 0; k < cnt; k++) {
        const uint8_t *en = e + 2 + (size_t)k * 12;
        if (exif_rd16(en, le) == 0x0112) {
            unsigned v = exif_rd16(en + 8, le);   /* count=1，值在 value 字段 */
            return (v >= 1 && v <= 8) ? (int)v : 0;
        }
    }
    return 0;
}

/* 只扫文件头 16KB 窗口（EXIF APP1 恒在段首附近，IFD0 在块头）；
 * 非 JPEG/无 EXIF/解析失败返回 0，合法值 1-8。 */
int photo_exif_orientation(const char *abs)
{
    enum { WIN = 16384 };
    FILE *f = fopen(abs, "rb");
    if (!f) return 0;
    uint8_t *buf = PHMALLOC(WIN);
    size_t n = buf ? fread(buf, 1, WIN, f) : 0;
    fclose(f);
    int ori = 0;
    if (buf && n >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (buf[i] != 0xFF) break;              /* 段同步丢失 */
            uint8_t mark = buf[i + 1];
            if (mark == 0xD8 || mark == 0x01
                || (mark >= 0xD0 && mark <= 0xD7)) { /* 无长度段 */
                i += 2;
                continue;
            }
            if (mark == 0xD9 || mark == 0xDA) break; /* EOI/SOS：头区止 */
            size_t seglen = ((size_t)buf[i + 2] << 8) | buf[i + 3];
            if (seglen < 2) break;
            if (mark == 0xE1 && i + 4 + 6 <= n
                && memcmp(buf + i + 4, "Exif\0\0", 6) == 0) {
                ori = exif_tiff_orientation(buf + i + 4 + 6,
                                            n - (i + 4 + 6));
                break;
            }
            i += 2 + seglen;
        }
    }
    if (buf) PHFREE(buf);
    return ori;
}

/* EXIF 方向 → lv_image 旋转角（0.1°）；2/4/5/7 镜像档不翻面，
 * 按最近纯旋转近似（竖图归正靠 90°，镜像细节 v1 不做） */
int photo_rot_from_exif(int orientation)
{
    switch (orientation) {
    case 3:
    case 4: return 1800;
    case 5:
    case 6:
    case 7: return 900;
    case 8: return 2700;
    default: return 0;                       /* 1/2/0 无旋转 */
    }
}

/* ---------------- 入口：缩略图/全屏 ---------------- */

/* 格式嗅探（读 8 字节魔数） */
static photo_fmt_t sniff_fmt(const char *abs)
{
    uint8_t m[8] = { 0 };
    FILE *f = fopen(abs, "rb");
    if (!f) return PH_FMT_UNKNOWN;
    size_t n = fread(m, 1, 8, f);
    fclose(f);
    if (n >= 3 && m[0] == 0xFF && m[1] == 0xD8) return PH_FMT_JPEG;
    if (n >= 8 && memcmp(m, "\x89PNG\r\n\x1a\n", 8) == 0)
        return PH_FMT_PNG;
    if (n >= 2 && m[0] == 'B' && m[1] == 'M') return PH_FMT_BMP;
    return PH_FMT_UNKNOWN;
}

/* 探测格式与尺寸（不解码）。0=OK / -1=失败。 */
int photo_probe(const char *abs, photo_fmt_t *fmt, int *w, int *h)
{
    *fmt = sniff_fmt(abs);
    *w = 0;
    *h = 0;
    if (*fmt == PH_FMT_PNG) {
        uint8_t ih[24];
        FILE *f = fopen(abs, "rb");
        if (!f) return -1;
        size_t n = fread(ih, 1, 24, f);
        fclose(f);
        if (n >= 24) {
            *w = (ih[16] << 24) | (ih[17] << 16) | (ih[18] << 8) | ih[19];
            *h = (ih[20] << 24) | (ih[21] << 16) | (ih[22] << 8) | ih[23];
            return 0;
        }
        return -1;
    }
    if (*fmt == PH_FMT_BMP) {
        FILE *f = fopen(abs, "rb");
        if (!f) return -1;
        bmp_hdr_t b;
        int ok = bmp_parse(f, &b) == 0;
        if (ok) *w = b.w;
        if (ok) *h = b.h;
        fclose(f);
        return ok ? 0 : -1;
    }
    if (*fmt == PH_FMT_JPEG) {
        JDEC jdec;
        uint8_t *work = PHMALLOC(JPEG_WORKBUF);
        jpeg_ctx_t ctx = { 0 };
        FILE *f = fopen(abs, "rb");
        int ok = 0;
        if (f && work) {
            ctx.f = f;
            if (jd_prepare(&jdec, jpeg_in, work,
                           JPEG_WORKBUF, &ctx) == JDR_OK) {
                *w = jdec.width;
                *h = jdec.height;
                ok = 1;
            }
        }
        if (work) PHFREE(work);
        if (f) fclose(f);
        return ok ? 0 : -1;
    }
    return -1;
}

/* 缩略图入口：缓存命中直返（缓冲归缓存所有，photo_cache_clear 后失效） */
const uint16_t *photo_thumb_get(const char *abs)
{
    int slot = thcache_find(abs);
    if (slot >= 0) {
        s_thumb_hit++;
        return s_thcache[slot].buf;
    }
    s_thumb_miss++;
    photo_fmt_t fmt = sniff_fmt(abs);
    if (fmt == PH_FMT_UNKNOWN) return NULL;
    uint8_t *cv = NULL;
    int cw = 0, ch = 0, bpp = 3;
    if (decode_to_canvas(abs, fmt, PH_THUMB_PX, 1,
                         &cv, &cw, &ch, &bpp) != 0) return NULL;
    uint16_t *buf = thcache_slot(abs);
    if (!buf) {
        PHFREE(cv);
        return NULL;
    }
    int dw = 0, dh = 0;
    conv_to_dst(cv, bpp, cw, ch, buf, PH_THUMB_PX, 1, &dw, &dh);
    PHFREE(cv);
    return buf;
}

/* 全屏解码。fill=1 cover 裁切 480²；fill=0 等比适配。
 * 输出实际尺寸 out_w / out_h。0=OK / -1=失败。 */
int photo_full_decode(const char *abs, int fill,
                      uint16_t *dst, int *out_w, int *out_h)
{
    s_full_n++;
    photo_fmt_t fmt = sniff_fmt(abs);
    if (fmt == PH_FMT_UNKNOWN) {
        s_fail_n++;
        return -1;
    }
    uint8_t *cv = NULL;
    int cw = 0, ch = 0, bpp = 3;
    if (decode_to_canvas(abs, fmt, PH_FULL_PX, 0,
                         &cv, &cw, &ch, &bpp) != 0) {
        s_fail_n++;
        return -1;
    }
    conv_to_dst(cv, bpp, cw, ch, dst,
                fill ? PH_FULL_PX : PH_DEC_MAX, fill, out_w, out_h);
    PHFREE(cv);
    return 0;
}

