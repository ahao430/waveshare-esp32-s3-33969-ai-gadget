/* photo_config — 相册设置持久化（双端，cJSON；data/photo/config.json，
 * temp+rename 原子替换，同 reader 进度文件纪律）。 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cJSON.h>

#include "photo.h"
#include "sd_paths.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define PLOG(fmt, ...) ESP_LOGI("photo", fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define PLOG(fmt, ...) do { printf(fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#endif

static const char *CFG_PATH = "data/photo/config.json";

void photo_config_load(photo_config_t *cfg)
{
    photo_config_t def = { 5, 0, 0, 0 };
    *cfg = def;
    char abs[PH_PATH_MAX];
    if (sd_resolve(abs, sizeof(abs), CFG_PATH) != 0) return;
    FILE *f = fopen(abs, "rb");
    if (!f) return;
    /* 配置文件很小，栈上 512B 足够 */
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    const cJSON *it = cJSON_GetObjectItem(root, "interval_sec");
    if (cJSON_IsNumber(it)) cfg->interval_sec = it->valueint;
    it = cJSON_GetObjectItem(root, "zoom_fill");
    if (cJSON_IsNumber(it)) cfg->zoom_fill = it->valueint ? 1 : 0;
    it = cJSON_GetObjectItem(root, "auto_play");
    if (cJSON_IsNumber(it)) cfg->auto_play = it->valueint ? 1 : 0;
    it = cJSON_GetObjectItem(root, "bg_dark");
    if (cJSON_IsNumber(it)) cfg->bg_dark = it->valueint ? 1 : 0;
    cJSON_Delete(root);
}

/* ---------------- 保存（temp+rename 原子替换） ---------------- */

void photo_config_save(const photo_config_t *cfg)
{
    char abs[PH_PATH_MAX];
    if (sd_resolve(abs, sizeof(abs), CFG_PATH) != 0) return;
    /* data/photo/ 目录幂等创建（同 reader_json_path 纪律） */
    char dir[PH_PATH_MAX];
    if (sd_resolve(dir, sizeof(dir), "data/photo") == 0) mkdir(dir, 0777);
    char tmp[PH_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", abs);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "interval_sec", cfg->interval_sec);
    cJSON_AddNumberToObject(root, "zoom_fill", cfg->zoom_fill);
    cJSON_AddNumberToObject(root, "auto_play", cfg->auto_play);
    cJSON_AddNumberToObject(root, "bg_dark", cfg->bg_dark);
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        fwrite(s, 1, strlen(s), f);
        free(s);
    }
    cJSON_Delete(root);
    fclose(f);
    rename(tmp, abs);
    PLOG("photo cfg saved: interval=%d fill=%d auto=%d bg=%d",
         cfg->interval_sec, cfg->zoom_fill, cfg->auto_play, cfg->bg_dark);
}
