/* sd_paths — TF 卡路径抽象（ADR-010，2026-11-16）。
 *
 * 固件：FATFS 挂载点 /sdcard（BSP bsp_sdcard_mount，1-bit SDMMC），插卡
 * 才可用；未插卡时所有文件功能降级（UI 空态），不影响其他功能。
 * 模拟器：目录取 GADGET_SD_DIR 环境变量，默认仓库根 sdcard/。
 *
 * 约定子目录（首次使用前调 ds_files_ensure_dirs()，固件在挂卡后调用）：
 *   backup/    配网自动备份（provision.json，含 Wi-Fi 明文密码，勿外借 TF）
 *   cache/     订阅/天气离线缓存（断网回读）
 *   data/      应用数据（番茄钟统计等；ADR-011 存储约定）
 *   appearance/ Web 自定义背景落盘（真机屏显需 LV_USE_PNG，暂只落盘）
 *   downloads/ 从 Bridge 下载的设备文件
 *   music/     本地音乐库（音乐播放器扫描 .mp3/.wav）
 *   Books/     本地小说库（阅读器扫描 .txt，递归深度 3；进度/配置在
 *              data/reader/*.json，不修改原始 TXT）  [ADR-017]
 */
#ifndef SD_PATHS_H
#define SD_PATHS_H

#include <stddef.h>

/* TF 卡根目录：固件 "/sdcard"；模拟器 GADGET_SD_DIR（默认 "sdcard"）。 */
const char *sd_root(void);

/* 把 rel（如 "cache/weather.json"）拼成绝对路径写入 out。
 * 返回 0=成功 / -1=缓冲不足。 */
int sd_resolve(char *out, size_t cap, const char *rel);

/* 建齐子目录（幂等； SD 不在时静默失败）。 */
void sd_ensure_dirs(void);

#endif /* SD_PATHS_H */