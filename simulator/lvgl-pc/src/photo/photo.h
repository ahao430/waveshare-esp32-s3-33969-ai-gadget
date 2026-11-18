/* photo — 本地相册核心层（双端：固件/模拟器，ADR-018）。
 *
 * 职责：
 *   - 扫描 picture/（根 + 一级子目录=分类），mtime 降序
 *   - 解码：JPEG（vendor TJpgDec R0.03，1/2..1/8 缩放解码）/
 *     PNG（lvgl 内置 lodepng，像素数上限 150K）/
 *     BMP（自研 24/32bpp 非压缩行流解析）
 *   - 缩略图 100×100 cover-crop / 全屏 ≤480² 适配，输出 RGB565
 *     （解码画布峰值 ≈ 0.72MB 单块，坑 #26/#27 账内）
 *   - JPEG EXIF 方向解析（APP1/TIFF IFD0 tag 0x0112）
 *   - 配置持久化 data/photo/config.json（cJSON，temp+rename）
 *
 * 线程模型（同 reader/music）：全部 IO/解码为阻塞操作——只能在
 * worker 线程调用；UI 线程只读 worker 产出的 RGB565 缓冲
 * （lv_async_call 交接）。EXIF 旋转不在解码层做（行窗流式无法
 * 转置），由 UI 层 lv_image_set_rotation 实现（映射 1-8 → 0/90/
 * 180/270，5/7 镜像档近似）。
 *
 * v1 边界：JPEG 任意尺寸（缩放解码）；PNG >150K 像素明确提示转
 * JPG；BMP 仅 24/32bpp 非压缩；GIF/WebP 不支持。
 */
#ifndef PHOTO_H
#define PHOTO_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define PH_PATH_MAX   220
#define PH_NAME_MAX   96
#define PH_LIST_MAX   256         /* 图片总数上限 */
#define PH_CAT_MAX    4           /* 分类 chips 上限（全部+≤4 不放一行） */
#define PH_THUMB_PX   100         /* 网格缩略图边长 */
#define PH_FULL_PX    480         /* 全屏显示边长（屏宽） */
#define PH_DEC_MAX    480         /* 全屏解码输出边长上限（=屏宽；dst ≤460KB） */
#define PH_PNG_MAXPX  160000      /* PNG 解码像素上限（RGBA8 中转 ≤640KB） */

typedef enum {
    PH_FMT_JPEG = 0,
    PH_FMT_PNG,
    PH_FMT_BMP,
    PH_FMT_UNKNOWN,
} photo_fmt_t;

typedef struct {
    char rel[PH_PATH_MAX];      /* sd 相对路径（进度/展示 key） */
    char name[PH_NAME_MAX];     /* 文件名（去目录） */
    long size;
    long mtime;                 /* unix 秒 */
    int  cat;                   /* 分类索引（-1=picture 根，未分类） */
} photo_item_t;

typedef struct {
    char name[40];              /* 子目录名（显示用，原样） */
    int  count;                 /* 该分类图片数 */
} photo_cat_t;

/* ---------------- 扫描（worker 调用） ---------------- */

/* 扫 picture/：根 + 一级子目录（≤PH_CAT_MAX 个，按名排序），
 * 图片条目按 mtime 降序。返回 0=成功（未插卡/空目录也 0）；
 * 条数填进两个计数指针参数（n_items / n_cats）。 */
int photo_scan(photo_item_t *items, int max_items, int *n_items,
               photo_cat_t *cats, int max_cats, int *n_cats);

/* ---------------- 解码（worker 调用） ---------------- */

/* 探测格式/尺寸（不解码：JPEG 用 jd_prepare 只扫头，
 * PNG 读 IHDR，BMP 读 DIB 头）。失败返回 -1。 */
int photo_probe(const char *abs, photo_fmt_t *fmt, int *w, int *h);

/* JPEG EXIF 方向：1-8；0=无 EXIF/非 JPEG/解析失败 */
int photo_exif_orientation(const char *abs);

/* EXIF 方向 → lv_image 旋转角（1/10 度；5/7 镜像档近似处理） */
int photo_rot_from_exif(int orientation);

/* 缩略图获取（LRU 缓存，未命中解码）。命中/新解码返回 RGB565
 * PH_THUMB_PX² 缓冲指针（缓存所有，photo_cache_clear 后失效）；
 * 失败返回 NULL。 */
const uint16_t *photo_thumb_get(const char *abs);

/* 全屏解码 → dst RGB565。fill=0 等比适配（输出 ≤PH_DEC_MAX²，
 * UI 用 LV_IMAGE_ALIGN_CONTAIN 收进屏）；fill=1 cover 裁切
 * PH_FULL_PX²。输出实际尺寸写进 out_w / out_h。 */
int photo_full_decode(const char *abs, int fill,
                      uint16_t *dst, int *out_w, int *out_h);

/* 解码层共享缓冲：thumb LRU 缓存 + 全屏 canvas（PSRAM 惰性，
 * UI on_exit 调 clear 全释放——PSRAM 让位语音 TTS 大块分配） */
void photo_cache_clear(void);

/* ---------------- 配置持久化（worker 调用） ---------------- */

typedef struct {
    int interval_sec;           /* 幻灯片间隔：3/5/10/15/30 */
    int zoom_fill;              /* 0=适应 1=填充（cover） */
    int auto_play;              /* 进全屏自动开始幻灯片 0/1 */
    int bg_dark;                /* 全屏背景 0=深蓝黑 1=纯黑 */
} photo_config_t;

void photo_config_load(photo_config_t *cfg);   /* 默认值兜底 */
void photo_config_save(const photo_config_t *cfg);

/* ---------------- 调试观测 ---------------- */

void photo_stats_get(int *thumb_hit, int *thumb_miss, int *full_n,
                     int *fail_n);
void photo_stats_reset(void);

#endif /* PHOTO_H */
