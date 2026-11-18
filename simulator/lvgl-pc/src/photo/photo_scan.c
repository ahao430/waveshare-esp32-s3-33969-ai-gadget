/* photo_scan - Pictures/ 目录扫描（双端；FATFS UTF-8 API 后中文名可见，
 * 坑 #28 已修）。只列 Pictures 根 + 一级子目录（<=4，按名排序）作为
 * 分类 chips 数据源；每目录只列一层不递归（"按目录扫描、不整卡扫描"）。
 * 条目 mtime 降序（最新优先）。 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "photo.h"
#include "sd_paths.h"

/* 有界拷贝（GCC 无法静态证明 snprintf %s 截断，用循环拷贝绕过
 * -Werror=format-truncation；dst 至少 1 字节） */
static void name_copy(char *dst, int cap, const char *src)
{
    int i = 0;
    for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* 名后缀 -> 格式；UNKNOWN=非图片 */
static photo_fmt_t fmt_of_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return PH_FMT_UNKNOWN;
    dot++;
    if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg"))
        return PH_FMT_JPEG;
    if (!strcasecmp(dot, "png")) return PH_FMT_PNG;
    if (!strcasecmp(dot, "bmp")) return PH_FMT_BMP;
    return PH_FMT_UNKNOWN;
}

/* 目录 rel 下收集图片条目（一层）。cat=-1 根（未分类）；
 * cat_idx>=0 时同步累加 cats[cat_idx].count。返回追加条数。 */
static int collect_dir(const char *dir_rel, int cat,
                       photo_item_t *items, int max, int *n,
                       photo_cat_t *cats, int cat_idx)
{
    char abs[PH_PATH_MAX];
    struct stat st;
    if (sd_resolve(abs, sizeof(abs), dir_rel) != 0) return 0;
    DIR *d = opendir(abs);
    if (!d) return 0;                       /* 目录不存在 = 0 条 */
    struct dirent *e;
    int added = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (fmt_of_name(e->d_name) == PH_FMT_UNKNOWN) continue;
        char rel[PH_PATH_MAX];
        if (snprintf(rel, sizeof(rel), "%s/%s", dir_rel, e->d_name)
            >= (int)sizeof(rel)) continue;
        if (sd_resolve(abs, sizeof(abs), rel) != 0) continue;
        if (stat(abs, &st) != 0) continue;  /* 坑 #28：stat 失败打日志的守卫同款 */
        if (*n >= max) break;
        photo_item_t *it = &items[(*n)++];
        snprintf(it->rel, sizeof(it->rel), "%s", rel);
        name_copy(it->name, (int)sizeof(it->name), e->d_name);
        it->size = (long)st.st_size;
        it->mtime = (long)st.st_mtime;
        it->cat = cat;
        if (cat_idx >= 0) cats[cat_idx].count++;
        added++;
    }
    closedir(d);
    return added;
}

/* 收集 Pictures 一级子目录名（<=max，按名排序）。返回个数。 */
static int list_subdirs(char names[][40], int max)
{
    char root[PH_PATH_MAX];
    if (sd_resolve(root, sizeof(root), "Pictures") != 0) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *ent;
    int nc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char rel[64];
        if (snprintf(rel, sizeof(rel), "Pictures/%s", ent->d_name)
            >= (int)sizeof(rel)) continue;
        char abs[PH_PATH_MAX];
        struct stat st;
        if (sd_resolve(abs, sizeof(abs), rel) != 0) continue;
        if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (nc < max) {
            name_copy(names[nc], 40, ent->d_name);
            nc++;
        }
    }
    closedir(d);
    /* 按名排序（稳定口径） */
    for (int a2 = 0; a2 < nc; a2++)
        for (int b2 = a2 + 1; b2 < nc; b2++)
            if (strcmp(names[a2], names[b2]) > 0) {
                char tmp[40];
                name_copy(tmp, 40, names[a2]);
                name_copy(names[a2], 40, names[b2]);
                name_copy(names[b2], 40, tmp);
            }
    return nc;
}

int photo_scan(photo_item_t *items, int max_items, int *n_items,
               photo_cat_t *cats, int max_cats, int *n_cats)
{
    *n_items = 0;
    *n_cats = 0;
    for (int i = 0; i < max_cats; i++) {
        cats[i].name[0] = 0;
        cats[i].count = 0;
    }
    char names[PH_CAT_MAX][40];
    int nc = list_subdirs(names, max_cats > PH_CAT_MAX ? PH_CAT_MAX : max_cats);
    /* 根目录未分类图片 + 各分类目录图片 */
    collect_dir("Pictures", -1, items, max_items, n_items, NULL, -1);
    for (int i = 0; i < nc; i++) {
        name_copy(cats[i].name, (int)sizeof(cats[i].name), names[i]);
        (*n_cats)++;
    }
    for (int i = 0; i < *n_cats; i++) {
        char rel[PH_PATH_MAX];
        if (snprintf(rel, sizeof(rel), "Pictures/%s", cats[i].name)
            >= (int)sizeof(rel)) continue;
        collect_dir(rel, i, items, max_items, n_items, cats, i);
    }
    /* mtime 降序（平局按 rel 名升序，稳定） */
    for (int a3 = 0; a3 < *n_items; a3++)
        for (int b3 = a3 + 1; b3 < *n_items; b3++)
            if (items[a3].mtime < items[b3].mtime
                || (items[a3].mtime == items[b3].mtime
                    && strcmp(items[a3].rel, items[b3].rel) > 0)) {
                photo_item_t tmp = items[a3];
                items[a3] = items[b3];
                items[b3] = tmp;
            }
    return 0;
}
