/* wifi_prov — 运行期配网（ADR-009）：Wi-Fi 凭据与 Bridge 地址存 NVS，
 * 编译期 wifi_secret.h 只作首次出厂兜底（NVS 有配置则完全忽略宏）。
 *
 * 调用线程约束：scan/connect 是阻塞操作（最坏 3s / 15s），必须在
 * 后台任务（栈 ≥8KB）调用；NVS 读写与状态查询任意线程可调。 */
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROV_PASS_MAX 64

typedef struct {
    /* 字段顺序/类型必须与 simulator 的 ds_wifi_ap_t 完全一致
     *（data_source_wifi.c 直接结构体转发） */
    char ssid[33];
    int8_t rssi;
    bool secure;     /* true=需要密码 */
    bool enterprise; /* true=企业认证（EAP），ESP32 默认不支持，UI 应标注 */
} wifi_prov_ap_t;

/* NVS Wi-Fi 凭据（"当前"网络，最近一次成功/配置的）：返回 false=从未配过。
 * save 同时把网络 upsert 进多网络列表（最多 6 个）。 */
bool wifi_prov_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
void wifi_prov_save(const char *ssid, const char *pass);

/* 多网络自动连接（家庭/公司多环境）：列表 ≥2 个才需要开机磋商。 */
bool wifi_prov_has_multi(void);
/* 开机磋商（独立任务调用）：3s 内没连上当前网络 → 扫描 → 按信号从
 * 已保存列表里挑在场的网络试连（成功即切换并记为当前）。
 * 返回 1=切换成功 / 0=无需切换或没找到 / -1=扫描失败。 */
int wifi_prov_autoconnect_others(void);

/* Bridge 地址（"http://host:port"）：返回 false = 未配置（调用方用
 * 编译期 BRIDGE_HOST 兜底）。存了就完全替代宏。 */
bool wifi_prov_bridge_get(char *out, size_t cap);
/* 保存并激活：加入地址簿（已存在则只激活）。 */
void wifi_prov_bridge_save(const char *url);
/* 编译期预置：把地址并进地址簿（幂等），不改当前激活地址；设备从未
 * 配置过地址时才同时设为默认激活项。用于 wifi_secret.h 的
 * BRIDGE_SEED_HOSTS（新服务器地址零输入进簿，切换仍在设备 UI 完成）。 */
void wifi_prov_bridge_seed(const char *url);
/* 地址簿（"\n" 分隔，首条=当前激活地址；簿空时返回编译期兜底单条）。
 * 返回 0=已写 out / -1=完全无地址。 */
int wifi_prov_bridge_list(char *out, size_t cap);
/* 从地址簿删除；当前激活地址不可删（返回 -1），未找到返回 -1。 */
int wifi_prov_bridge_remove(const char *url);

/* ---- TF 自动备份/恢复（ADR-010，2026-11-16）----
 * save/remove 后自动写 /sdcard/backup/provision.json（未插卡静默跳过）；
 * 开机时若 NVS 无凭据可调 restore 恢复（刷机/擦 NVS 免重配）。
 * 备份含 Wi-Fi 明文密码——TF 卡勿外借（README/DEPLOY 注明）。 */
bool wifi_prov_restore_from_backup(void); /* true=已恢复 */
void wifi_prov_backup_now(void);          /* 手动触发备份（一般不需要） */

/* 阻塞扫描（约 1.5~3s，内部 block 模式）。
 * 返回 0=成功（*n 条，按信号强度降序、同名 SSID 去重）/
 * -1=Wi-Fi 忙（正在连接中，稍后再试）/ -2=驱动失败。 */
int wifi_prov_scan(wifi_prov_ap_t *out, int max, int *n);

/* 连接尝试（阻塞，上限 15s）：先试连、成功才写 NVS；失败自动恢复
 * 旧凭据重连（设备不失控）。要求 Wi-Fi 已 start（wifi_start 在无凭据
 * 时也会 start 供扫描）。
 * 返回 0=已连上 / -1=失败（密码错/超时）。 */
int wifi_prov_connect(const char *ssid, const char *pass);

/* 当前连接状态：true=已连接（写出 ssid 与点分 IP） */
bool wifi_prov_status(char *ssid, size_t ssid_cap, char *ip, size_t ip_cap);

/* 配网进行中：main.c 的 wifi_evt 自动重连在配网期间必须避让，
 * 否则旧凭据的 connect 会和试连新网互相打断。 */
bool wifi_prov_busy(void);

/* 手动模式（Wi-Fi 设置面板打开期间）：暂停自动重连，避免开机重连循环
 * 每 1.5s 抢驱动导致扫描被吞（返回 OK 但 0 结果，2026-11-16 真机）；
 * on=false 时若有凭据且未连接立即重连。 */
void wifi_prov_manual_mode(bool on);
bool wifi_prov_manual(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROV_H */
