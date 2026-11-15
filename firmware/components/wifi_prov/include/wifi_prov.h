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
    char ssid[33];
    int8_t rssi;
    bool secure; /* true=需要密码 */
} wifi_prov_ap_t;

/* NVS Wi-Fi 凭据：返回 false = 从未配过（ssid[0]='\0'） */
bool wifi_prov_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
void wifi_prov_save(const char *ssid, const char *pass);

/* Bridge 地址（"http://host:port"）：返回 false = 未配置（调用方用
 * 编译期 BRIDGE_HOST 兜底）。存了就完全替代宏。 */
bool wifi_prov_bridge_get(char *out, size_t cap);
/* 保存并激活：加入地址簿（已存在则只激活）。 */
void wifi_prov_bridge_save(const char *url);
/* 地址簿（"\n" 分隔，首条=当前激活地址；簿空时返回编译期兜底单条）。
 * 返回 0=已写 out / -1=完全无地址。 */
int wifi_prov_bridge_list(char *out, size_t cap);
/* 从地址簿删除；当前激活地址不可删（返回 -1），未找到返回 -1。 */
int wifi_prov_bridge_remove(const char *url);

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

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROV_H */
