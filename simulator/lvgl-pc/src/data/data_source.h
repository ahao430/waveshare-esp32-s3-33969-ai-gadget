/* data_source — UI 与数据源之间的唯一接口（ADR-001 关键缝隙）。
 * 模拟器与固件共用同一 UI 代码：
 *   - PC：mock 实现 / bridge 实现（HTTP 到 Go Bridge）
 *   - 固件：bridge 实现（esp_http_client）+ 设备本地状态（电池/Wi-Fi）
 * 电池/Wi-Fi 属于设备本地数据，固件上来自 power/network 服务；
 * 模拟器上始终由 mock 提供数值。
 */
#ifndef DATA_SOURCE_H
#define DATA_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#define DS_SERVER_MAX 6

typedef struct {
    char name[16];
    bool online;
    int cpu_pct;
    int mem_pct;
    int disk_pct;
    int net_up_kbps;   /* 上行 KB/s（bps/1024 换算，UI 显示用） */
    int net_down_kbps; /* 下行 KB/s */
    int uptime_h; /* 小时粒度，设备端够用 */
} gadget_server_t;

#define DS_SUB_MAX 8

/* 一个订阅（Provider）的额度视图（PLAN §5.1：单 App 多订阅左右切换） */
typedef struct {
    char name[24];       /* 显示名（如 "智谱" "Kimi"） */
    char plan[48];
    int used_5h;         /* % */
    int used_week;       /* % */
    int win_5h_s;        /* 5h 窗口总秒数（18000；换算剩余小时用） */
    int week_s;          /* 周窗口总秒数（604800；换算 x/y 小时用） */
    long reset_5h;       /* unix 秒 */
    long reset_week;     /* unix 秒 */
    bool valid;
} ds_subscription_t;

typedef struct {
    /* 订阅列表（carousel 数据源） */
    ds_subscription_t subs[DS_SUB_MAX];
    int sub_count;

    /* 兼容旧引用（保留至 quota 页重写完成） */
    int zhipu_used_5h;
    int zhipu_used_week;
    long zhipu_reset_5h;
    long zhipu_reset_week;
    char zhipu_plan[48];

    int kimi_used_5h;
    int kimi_used_week;

    /* 服务器监控（Komari 经 Bridge 聚合，设备收摘要） */
    gadget_server_t servers[DS_SERVER_MAX];
    int server_count;

    /* 设备本地状态 */
    int battery_pct;
    bool wifi_ok;
    long updated_unix; /* 额度最近一次成功刷新时间 */

    /* 电源（设备本地：模拟器=mock（GADGET_DEMO_BATTERY 可配）；固件=AXP2101
     * 经 Power Manager 轮询回填，PLAN §13.3。UI 统一走状态栏规则表） */
    bool battery_present;
    bool usb_connected;
    bool charging;
    int battery_voltage_mv;

    /* 设备设置（模拟器/内存；真机走 device_config NVS，Phase 6） */
    int brightness;         /* 10~100 */
    int volume;             /* 0~100 */
    int display_timeout_s;      /* 息屏超时（PLAN §13.1 档位） */
    int sub_refresh_interval_s; /* 订阅自动刷新间隔（PLAN §6.3：5~60 分钟） */
    int theme_idx;          /* UI 主题（ui_theme k_ui_themes 下标） */
} gadget_state_t;

/* 初始化。source: "mock" 或 "bridge"；bridge_url 如 http://127.0.0.1:8081；
 * provider: bridge 模式下抓取的 provider id（mock/zhipu）。
 * 参数为 NULL 时读环境变量 GADGET_DATA_SOURCE / GADGET_BRIDGE_URL /
 * GADGET_BRIDGE_PROVIDER，最终默认 mock。 */
void ds_init(const char *source, const char *bridge_url, const char *provider);

/* 当前状态（UI 只读；实现内部保证有合理初值） */
const gadget_state_t *ds_state(void);

/* 拉取一次数据。
 * Phase 1 简化：同步阻塞（bridge 模式超时约 1.5s）。
 * 返回 true=本次成功；false=失败但旧数据仍可用（UI 显示过期状态）。
 * TODO(固件)：换 esp_http_client 异步 + 事件回调。 */
bool ds_refresh(void);

/* 人读名称，如 "mock" / "bridge(http://127.0.0.1:8081/mock)" */
const char *ds_source_name(void);

/* 智谱预热（PLAN §5.6）：调用 Bridge POST /quota/zhipu/prewarm（幂等 Key
 * 由实现生成）。结果说明写入 msg（含 skipped_* 原因）。UI 必须二次确认
 * 后才可调用；预热不承诺重置 5 小时窗口。 */
typedef enum {
    DS_PREWARM_OK = 0,
    DS_PREWARM_UNSUPPORTED,
    DS_PREWARM_ERROR,
} ds_prewarm_result_t;

ds_prewarm_result_t ds_prewarm_zhipu(char *msg, int msg_len);

/* Bigfish Chat（PLAN §5.3/§7.4）：经 Bridge Model Router 发一轮对话。
 * 会话历史由调用方（UI）维护，Bridge 无状态。流式：每个增量调
 * on_delta（可为 NULL）；完整回复写入 reply_full。超时较长（LLM 生成），
 * 必须在后台线程调用，UI 用轮询/事件消费增量。 */
typedef struct {
    const char *role;    /* "user" / "assistant" */
    const char *content;
} ds_chat_msg_t;

typedef enum {
    DS_CHAT_OK = 0,
    DS_CHAT_UNSUPPORTED,
    DS_CHAT_ERROR,
} ds_chat_result_t;

typedef void (*ds_chat_delta_cb_t)(const char *delta, void *user);

ds_chat_result_t ds_chat_send_stream(const ds_chat_msg_t *msgs, int n,
                                     ds_chat_delta_cb_t on_delta, void *user,
                                     char *reply_full, int reply_len);

/* 语音一步式交互（PLAN §5.3）：录音 PCM → Bridge（ASR→LLM→TTS）。
 * 返回 transcript（听到的话）+ reply（LLM 回复文本）。
 * 音频播放数据当前 mock（Bridge 返回 WAV 但模拟器不真播，字幕推进）。
 * 必须在后台线程调用。 */
typedef enum {
    DS_VOICE_OK = 0,
    DS_VOICE_UNSUPPORTED,
    DS_VOICE_ERROR,
} ds_voice_result_t;

ds_voice_result_t ds_voice_interact(const int16_t *pcm, int frames,
                                    char *transcript, int transcript_len,
                                    char *reply, int reply_len);

/* 设备设置更新（模拟器内存态；真机= PUT 设备 config API，Phase 2/6） */
void ds_config_update(int brightness, int volume, int timeout_s);
void ds_config_set_refresh_interval(int seconds); /* 订阅自动刷新间隔 */
int ds_config_refresh_interval(void);
void ds_config_set_theme(int idx);                /* UI 主题 */

/* 自定义背景（Web 上传到 Bridge，PLAN §8 外观）：
 * 尝试拉取 Bridge 背景并写入 save_path（建议 simulator 相对路径）。
 * 返回 0=已写入新文件；1=无自定义背景（保留旧文件）；-1=网络失败。 */
int ds_appearance_fetch(const char *save_path);

/* 拉取 Web 下发的主题（Bridge appearance/theme）；返回 0=拿到（写 *theme）
 * / 1=未配置默认 / -1=失败。bridge 模式有效。 */
int ds_appearance_fetch_theme(int *theme);

/* 聊天 provider：
 * - ds_chat_providers 填充 "\n" 分隔的 provider id 列表（bridge=HTTP
 *   拉取，mock 返回 "mock"）；在后台线程调用。
 * - ds_chat_set_provider 选择发送用的 provider 前缀；ds_chat_provider()
 *   取当前值。 */
int ds_chat_providers(char *out, int cap);
void ds_chat_set_provider(const char *id);
const char *ds_chat_provider(void);

#endif /* DATA_SOURCE_H */
