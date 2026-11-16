/* data_source_files — TF 卡文件管理数据源（ADR-010，2026-11-16）。
 *
 * 单文件双平台（模式同 data_source_wifi.c）：
 *   固件   = FATFS(/sdcard) + Bridge HTTP（http_client.c）
 *   模拟器 = GADGET_SD_DIR 目录 + 同一套 HTTP（可连真 Bridge 调试）
 *
 * 调用线程：全部阻塞（目录扫描、整包 HTTP、文件读写），只能在后台
 * worker 线程调用（16KB 栈，同 wx_worker）；LVGL 线程禁直接调用
 * （AGENTS 踩坑 #11：LVGL 线程内禁一切阻塞调用）。
 */
#include "data_source.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "http_client.h"
#include "sd_paths.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define FLOG(fmt, ...) ESP_LOGI("ds_files", fmt, ##__VA_ARGS__)
#else
#define FLOG(fmt, ...) ((void)0)
#endif

/* 单文件上限：PSRAM 整包缓冲水位（与语音 256KB 同量级，留足余量）。 */
#define DS_FILE_MAX_BYTES (2 * 1024 * 1024)
#define DS_FILE_HTTP_TIMEOUT_MS 10000

const char *sd_root(void)
{
#ifdef ESP_PLATFORM
    return "/sdcard";
#else
    const char *env = getenv("GADGET_SD_DIR");
    return (env && env[0]) ? env : "sdcard";
#endif
}

int sd_resolve(char *out, size_t cap, const char *rel)
{
    int n = snprintf(out, cap, "%s/%s", sd_root(), rel);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

void sd_ensure_dirs(void)
{
    static const char *subs[] = { "backup", "cache", "appearance", "downloads" };
    char path[160];
    (void)mkdir(sd_root(), 0777); /* 模拟器根目录可能不存在；固件是挂载点（已存在） */
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        if (sd_resolve(path, sizeof(path), subs[i]) == 0) {
            (void)mkdir(path, 0777); /* 已存在返回 -1，忽略 */
        }
    }
}

/* 大块缓冲：固件优先 PSRAM（内部 RAM 紧），模拟器普通 malloc。 */
static void *file_buf_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static void file_buf_free(void *p) { free(p); }

/* ---------------- 本地目录 ---------------- */

static int entry_cmp(const void *a, const void *b)
{
    const ds_file_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1; /* 目录优先 */
    return strcasecmp(x->name, y->name);
}

int ds_files_list(const char *dir, ds_file_entry_t *out, int max, int *n)
{
    if (!dir || !out || !n || max <= 0) return -1;
    *n = 0;
    DIR *d = opendir(dir);
    if (!d) return -1; /* 未插卡 / 路径不存在 */

    struct dirent *de;
    char path[256];
    while ((de = readdir(d)) != NULL && *n < max) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) continue;
        ds_file_entry_t *e = &out[*n];
        strncpy(e->name, de->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->is_dir = S_ISDIR(st.st_mode);
        e->size = (long)st.st_size;
        e->mtime = (long)st.st_mtime;
        (*n)++;
    }
    closedir(d);
    qsort(out, (size_t)*n, sizeof(out[0]), entry_cmp);
    return 0;
}

int ds_files_delete(const char *path)
{
    if (!path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return -1; /* 一期不递归删目录 */
    return remove(path) == 0 ? 0 : -1;
}

/* ---------------- 名称编码（远端 URL） ---------------- */

static int url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) return -1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xf];
        }
    }
    if (o >= cap) return -1;
    out[o] = '\0';
    return 0;
}

/* ---------------- Bridge 互通 ---------------- */

static int bridge_files_url(char *out, size_t cap, const char *name)
{
    char host[160];
    if (!ds_bridge_host_get(host, sizeof(host))) return -1;
    char enc[400];
    if (name && url_encode(name, enc, sizeof(enc)) != 0) return -1;
    int n = name ? snprintf(out, cap, "%s/api/v1/files/%s", host, enc)
                 : snprintf(out, cap, "%s/api/v1/files", host);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static void set_msg(char *msg, int cap, const char *pfx, const char *name)
{
    if (!msg || cap <= 0) return;
    snprintf(msg, (size_t)cap, "%s%s", pfx, name ? name : "");
}

int ds_files_upload(const char *path, char *msg, int msg_cap)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        if (msg) snprintf(msg, (size_t)msg_cap, "文件不存在");
        return -1;
    }
    if (st.st_size <= 0 || st.st_size > DS_FILE_MAX_BYTES) {
        if (msg) snprintf(msg, (size_t)msg_cap, "大小超限（≤2MB）");
        return -1;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char url[512];
    if (bridge_files_url(url, sizeof(url), base) != 0) {
        if (msg) snprintf(msg, (size_t)msg_cap, "Bridge 地址未配置");
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (msg) snprintf(msg, (size_t)msg_cap, "打开失败");
        return -1;
    }
    char *buf = file_buf_alloc((size_t)st.st_size);
    if (!buf) {
        fclose(f);
        if (msg) snprintf(msg, (size_t)msg_cap, "内存不足");
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) {
        file_buf_free(buf);
        if (msg) snprintf(msg, (size_t)msg_cap, "读取失败");
        return -1;
    }

    int status = http_post_raw_stream(url, "application/octet-stream", buf, rd,
                                      NULL, NULL, 0, NULL, NULL, 0,
                                      NULL, NULL, DS_FILE_HTTP_TIMEOUT_MS);
    file_buf_free(buf);
    FLOG("upload %s status=%d", base, status);
    if (status == 200) {
        set_msg(msg, msg_cap, "已上传 ", base);
        return 0;
    }
    if (msg) snprintf(msg, (size_t)msg_cap, "上传失败（HTTP %d）", status);
    return -1;
}

int ds_files_remote_list(char *out, int cap)
{
    char url[256];
    if (bridge_files_url(url, sizeof(url), NULL) != 0) return -1;
    char *buf = malloc(64 * 1024); /* JSON 列表；堆分配防炸栈 */
    if (!buf) return -1;
    int status = http_get(url, buf, 64 * 1024, DS_FILE_HTTP_TIMEOUT_MS);
    if (status != 200) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;
    const cJSON *files = cJSON_GetObjectItem(root, "files");
    size_t off = 0;
    out[0] = '\0';
    const cJSON *it;
    cJSON_ArrayForEach(it, files) {
        const cJSON *name = cJSON_GetObjectItem(it, "name");
        const cJSON *size = cJSON_GetObjectItem(it, "size");
        if (!cJSON_IsString(name)) continue;
        int w = snprintf(out + off, (size_t)cap - off, "%s%s\t%ld",
                         off ? "\n" : "", name->valuestring,
                         cJSON_IsNumber(size) ? (long)size->valuedouble : 0);
        if (w <= 0 || (size_t)w >= (size_t)cap - off) break;
        off += (size_t)w;
    }
    cJSON_Delete(root);
    return 0;
}

int ds_files_download(const char *name, char *msg, int msg_cap)
{
    if (!name || !name[0]) return -1;
    char url[512];
    if (bridge_files_url(url, sizeof(url), name) != 0) {
        if (msg) snprintf(msg, (size_t)msg_cap, "Bridge 地址未配置");
        return -1;
    }
    char *buf = file_buf_alloc(DS_FILE_MAX_BYTES);
    if (!buf) {
        if (msg) snprintf(msg, (size_t)msg_cap, "内存不足");
        return -1;
    }
    size_t len = 0;
    int status = http_get_bin(url, buf, DS_FILE_MAX_BYTES, &len, DS_FILE_HTTP_TIMEOUT_MS);
    if (status != 200 || len == 0) {
        file_buf_free(buf);
        if (msg) snprintf(msg, (size_t)msg_cap, "下载失败（HTTP %d）", status);
        return -1;
    }
    /* 只取文件名部分（远端平铺目录，name 已由服务端校验无分隔符） */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    char path[320];
    snprintf(path, sizeof(path), "%s/downloads/%s", sd_root(), base);
    FILE *f = fopen(path, "wb");
    if (!f) {
        file_buf_free(buf);
        if (msg) snprintf(msg, (size_t)msg_cap, "写 TF 失败");
        return -1;
    }
    size_t wr = fwrite(buf, 1, len, f);
    fclose(f);
    file_buf_free(buf);
    FLOG("download %s len=%u -> %s wr=%u", base, (unsigned)len, path, (unsigned)wr);
    if (wr != len) {
        if (msg) snprintf(msg, (size_t)msg_cap, "写入不完整");
        return -1;
    }
    set_msg(msg, msg_cap, "已下载 ", base);
    return 0;
}

/* ---- data_source.h 对外包装（sd_paths 为数据层内部接口） ---- */
const char *ds_sd_root(void) { return sd_root(); }

int ds_sd_resolve(char *out, int cap, const char *rel)
{
    return sd_resolve(out, (size_t)cap, rel);
}

void ds_files_ensure_dirs(void) { sd_ensure_dirs(); }
