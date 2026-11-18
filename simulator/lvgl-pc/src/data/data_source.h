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

#define DS_SUB_MAX 8

/* 一个订阅（Provider）的额度视图（PLAN §5.1：单 App 多订阅左右切换）。
 * 两种形态：窗口型（win_5h_s>0，coding plan 卡）；余额型（win_5h_s==0
 * 且 balance 非空，按量计费卡——DeepSeek 余额查询，无窗口/预热概念）。 */
typedef struct {
    char name[24];       /* 显示名（如 "智谱" "Kimi"） */
    char plan[48];       /* 套餐行（智谱已含"· X月X日到期"） */
    char balance[20];    /* 余额文本（如 "¥110.35"；仅余额型） */
    int used_5h;         /* % */
    int used_week;       /* % */
    int win_5h_s;        /* 5h 窗口总秒数（18000；0=无窗口=余额型） */
    int week_s;          /* 周窗口总秒数（604800） */
    long reset_5h;       /* unix 秒 */
    long reset_week;     /* unix 秒 */
    bool can_prewarm;    /* provider 支持预热（现仅智谱） */
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

    /* 设备本地状态 */
    int battery_pct;
    bool wifi_ok;
    long updated_unix; /* 额度最近一次成功刷新时间 */
    bool subs_cache;   /* 订阅数据来自 TF 离线缓存（网络拉取失败，UI 提示离线） */

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

/* app：调用方 App 标识（"chat"/"xiaozhi"/"muse"），Bridge 按
 * App 选择 TTS 引擎/音色；NULL 或空 = 默认（第一个机器人实例）。 */
ds_voice_result_t ds_voice_interact(const char *app, const int16_t *pcm, int frames,
                                    char *transcript, int transcript_len,
                                    char *reply, int reply_len);

/* 设备设置更新（内存态；真机亮度/音量另经 ds_config_persist 回写 Bridge
 * 跨重启持久化）。brightness/volume/timeout_s 传 0/-1 表示该项不改。 */
void ds_config_update(int brightness, int volume, int timeout_s);
void ds_config_set_refresh_interval(int seconds); /* 订阅自动刷新间隔 */
int ds_config_refresh_interval(void);

/* UI 改动亮度/音量/息屏超时后调用：bridge 模式防抖 1.5s 合并 PUT
 * /api/v1/device/config（一轮拖动只回写一次）；mock 模式 no-op。
 * 只在 UI（LVGL）线程调用；网络在内部 worker，不阻塞调用方。 */
void ds_config_persist(void);

/* 设备运行配置下发（Bridge /api/v1/device/config，启动/联网时拉取）：
 * 0=成功 / -1=失败。无论成败 out 都被置为缺省（sub=0、bright/vol=-1
 * =未下发，保持本地值），实现只覆盖拿到的字段；亮度/音量拉到后需
 * 应用到硬件（ui_statusbar_apply_hw）。 */
typedef struct {
    int sub_refresh_interval_s; /* 0=未下发 */
    int brightness;             /* -1=未下发 */
    int volume;                 /* -1=未下发 */
} ds_device_config_t;
int ds_device_config_fetch(ds_device_config_t *out);
void ds_config_set_theme(int idx);

/* 清空云端对话记忆（AstrBot username+session 轮换；bot_id 空=全部实例）。
 * 返回 0=成功 / -1=失败（mock=成功空操作）。设备端"新对话"按钮：
 * 本地气泡 + 云端记忆一起清，只影响指定机器人。 */
int ds_chat_reset(const char *bot_id);

/* 自定义背景（Web 上传到 Bridge，PLAN §8 外观）：
 * 尝试拉取 Bridge 背景并写入 save_path（建议 simulator 相对路径）。
 * 返回 0=已写入新文件；1=无自定义背景（保留旧文件）；-1=网络失败。 */
int ds_appearance_fetch(const char *save_path);

/* 拉取 Web 下发的主题（Bridge appearance/theme）；返回 0=拿到（写 *theme）
 * / 1=未配置默认 / -1=失败。bridge 模式有效。 */
int ds_appearance_fetch_theme(int *theme);

/* 天气快照（Bridge /api/v1/weather → Open-Meteo；mock 固定演示数据） */
typedef struct {
    char date[12]; /* YYYY-MM-DD */
    char desc[12];
    int tmax, tmin;
} ds_weather_day_t;

typedef struct {
    bool valid;
    char location[32];
    char desc[24];
    char category[8]; /* sun|cloud|fog|rain|snow|thunder（UI 配色） */
    int temp_c, feels_c, humidity, wind_kmh;
    long updated_unix;
    int day_n;
    ds_weather_day_t days[5];
} ds_weather_t;

/* 拉取天气：0=成功写 *out / 1=数据源不支持 / -1=失败（*out.valid=false） */
int ds_weather_fetch(ds_weather_t *out);

/* 拉取 Web 下发的主页排布（Bridge /api/v1/launcher）：0=成功（out_csv
 * 写成 "chat,xiaozhi,..." 逗号分隔 key 列表，*count=App 数）/
 * 1=未配置默认 / -1=失败。bridge 模式有效。 */
int ds_launcher_fetch(char *out_csv, int cap, int *count);

/* 拉取 Web 下发的按键映射（Bridge /api/v1/keys）：0=四槽写入（每槽 ≥8
 * 字节）/ 1=未配置默认（不写） / -1=失败。bridge 模式有效。 */
int ds_keys_fetch(char *pwr_short, char *boot_short,
                  char *key3_short, char *key3_long, int cap);

/* 聊天 provider：
 * - ds_chat_providers 填充 "\n" 分隔的 provider id 列表（bridge=HTTP
 *   拉取，mock 返回 "mock"）；在后台线程调用。
 * - ds_chat_set_provider 选择发送用的 provider 前缀；ds_chat_provider()
 *   取当前值。 */
int ds_chat_providers(char *out, int cap);
void ds_chat_set_provider(const char *id);
const char *ds_chat_provider(void);

/* AstrBot 机器人实例（多实例切换，设备对话页点标题轮换）：
 * - ds_chat_bots: 每行 "id\tlabel"（bridge 拉 /api/v1/bots；0=OK）
 * - ds_chat_active_id: 当前机器人 id（bridge /api/v1/chat/active；0=OK）
 * - ds_chat_set_active: 切换当前机器人（PUT /api/v1/chat/active；0=OK）
 * 均需在后台线程调用（HTTP）。 */
int ds_chat_bots(char *out, int cap);
int ds_chat_active_id(char *out, int cap);
int ds_chat_set_active(const char *id);

/* ---- 运行期配网（设备本地能力，ADR-009；实现 data_source_wifi.c，
 * 单文件双平台：固件=NVS+esp_wifi 真实现，模拟器=演示数据） ---- */
typedef struct {
    /* 注意：字段顺序/类型必须与 firmware wifi_prov_ap_t 完全一致
     *（固件分支直接结构体转发；曾因 int/int8_t 不一致导致 rssi 全 0） */
    char ssid[33];
    int8_t rssi;
    bool secure;     /* true=需要密码 */
    bool enterprise; /* true=企业认证（EAP），设备不支持连接 */
} ds_wifi_ap_t;

/* 扫描周围 Wi-Fi（阻塞 1.5~3s，后台线程调用）。
 * 返回 0=成功（*n 条）/ -1=Wi-Fi 忙（连接中）/ -2=失败。 */
int ds_wifi_scan(ds_wifi_ap_t *out, int max, int *n);

/* 连接并保存（阻塞 ≤15s，后台线程调用）：0=已连上 / -1=失败
 * （密码错/超时；失败自动恢复旧网络）。密码为空 = 开放网络。 */
int ds_wifi_connect(const char *ssid, const char *pass);

/* 当前连接状态：true=已连接（写出 SSID 与 IP）；未连接时置空串。 */
bool ds_wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap);

/* Bridge 地址（http://host:port）。get 返回 false=未配置（编译期
 * wifi_secret.h 兜底）；save 立即生效（下次 HTTP 请求即用新地址）。
 * 配网页保存地址后调 ds_bridge_rebind 让 bridge 数据源的热 URL 全部
 * 重算（无需重启）。
 *
 * 多地址（调试/多环境）：save 会把地址加入地址簿；list 列出全部已保存
 * 地址（"\n" 分隔，含编译期兜底时首条为当前生效地址）；remove 删除
 * 非当前地址。UI 切换 = save(目标) + rebind。 */
bool ds_bridge_host_get(char *out, int cap);
void ds_bridge_host_save(const char *url);
/* 当前激活的 Bridge 基址（http://host:port；mock 模式为空串）。
 * 网络诊断等需要直连探测的 UI 用。 */
const char *ds_bridge_active_base(void);

void ds_bridge_rebind(const char *url);
int ds_bridge_host_list(char *out, int cap); /* 0=已写 / -1=空 */
int ds_bridge_host_remove(const char *url);  /* 0=已删 / -1=当前或未找到 */

/* 自定义背景落盘路径（Web 上传 → Bridge → 设备拉取缓存）：
 * 固件写 TF（屏显解码需 LV_USE_PNG，本期只落盘，见 ADR-010）；
 * 模拟器沿用仓库相对路径。 */
#ifdef ESP_PLATFORM
#define DS_CUSTOM_BG_PATH "/sdcard/appearance/custom-bg.png"
#else
#define DS_CUSTOM_BG_PATH "simulator/lvgl-pc/assets/custom-bg.png"
#endif

/* ---- TF 卡文件（ADR-010；实现 data_source_files.c，单文件双平台：
 * 固件=FATFS(/sdcard)+Bridge HTTP；模拟器=GADGET_SD_DIR 目录+同一 HTTP）。
 * 全部阻塞（目录扫描/整包 HTTP/文件读写）——只能在后台 worker 线程调用
 * （16KB 栈），LVGL 线程禁直接调用（AGENTS 踩坑 #11）。 ---- */
#define DS_FILE_NAME_MAX 96
typedef struct {
    char name[DS_FILE_NAME_MAX];
    long size;
    long mtime; /* unix 秒 */
    bool is_dir;
} ds_file_entry_t;

/* TF 根目录（固件 "/sdcard"；模拟器 GADGET_SD_DIR，默认 "sdcard"） */
const char *ds_sd_root(void);
/* rel（如 "cache/weather.json"）→ 绝对路径；0=OK / -1=缓冲不足 */
int ds_sd_resolve(char *out, int cap, const char *rel);
/* 建齐 backup/cache/appearance/downloads 子目录（幂等，启动调用） */
void ds_files_ensure_dirs(void);

/* 列目录（目录优先+名称排序）：0=成功（*n 条）/ -1=打不开（未插卡等） */
int ds_files_list(const char *dir, ds_file_entry_t *out, int max, int *n);
/* 删除文件（不递归删目录）：0/-1 */
int ds_files_delete(const char *path);
/* TF → Bridge 上传（≤2MB；msg 回显结果文案）：0/-1 */
int ds_files_upload(const char *path, char *msg, int msg_cap);
/* Bridge 文件列表 → out 每行 "name\tsize"（"\n" 分隔）：0=成功 */
int ds_files_remote_list(char *out, int cap);
/* Bridge → TF downloads/<name>（≤2MB；msg 回显结果文案）：0/-1 */
int ds_files_download(const char *name, char *msg, int msg_cap);

/* ---- 文件管理器操作（ADR-019，全部 worker 线程调用；rel 都是
 * sd 相对路径，如 "Pictures/a.jpg"；失败 -1 且 msg 填原因） ---- */
/* 新建目录（幂等失败）：0/-1 */
int ds_files_mkdir(const char *dir_rel, char *msg, int msg_cap);
/* 同目录改名（new_name 仅文件名，不含路径）：0/-1 */
int ds_files_rename(const char *rel, const char *new_name,
                    char *msg, int msg_cap);
/* 移动（FATFS rename 跨目录）/ 复制（16KB 分块读写，≤8MB）：0/-1 */
int ds_files_move(const char *src_rel, const char *dst_dir_rel,
                  char *msg, int msg_cap);
int ds_files_copy(const char *src_rel, const char *dst_dir_rel,
                  char *msg, int msg_cap);
/* 删除；recursive=1 时空目录也删（非空目录仍拒绝，防误删整树）：
 * 0/-1（msg 填原因，如"目录非空"） */
int ds_files_delete2(const char *rel, int recursive, char *msg, int msg_cap);
/* TF 容量（字节）：0=成功（total/free 填充；未插卡 -1） */
int ds_files_capacity(long *total, long *free_b);

/* ---- 收藏夹（data/files/fav.json，rel 路径数组，原子写） ---- */
#define DS_FAV_MAX 64
/* 加/移收藏（toggle，返回操作后是否收藏；-1=存盘失败/满） */
int ds_files_fav_toggle(const char *rel);
/* 是否已收藏 */
int ds_files_fav_has(const char *rel);
/* 列出全部收藏（rel 拷进 out；返回条数） */
int ds_files_fav_list(char out[][DS_FILE_NAME_MAX * 2], int max);

/* ---------------- 设备状态采集（设置→系统状态仪表盘） ----------------
 * 阻塞采集（TF FATFS 扫描/Flash 分区遍历，~几十毫秒到几百毫秒），
 * 必须在 worker 线程调用（坑 #11），不在 LVGL 线程。
 * 真机：heap_caps 水位/温度传感器/分区表/自旋采样负载；
 * 模拟器：设计稿量级的演示值（TF 卡为 sdcard/ 目录真实占用）。 */
typedef struct {
    bool valid; /* 采集成功（任一项可用即 true） */
    /* SRAM（内部 RAM，字节） */
    uint32_t sram_free, sram_min, sram_total;
    /* PSRAM（字节） */
    uint32_t psram_free, psram_total;
    /* Flash：app 分区（程序）+ storage 分区（内置 FATFS，未挂载时
     * used=0 且 flash_fs_mounted=false，容量仍给出） */
    uint32_t flash_app_used, flash_app_total;
    uint32_t flash_fs_used, flash_fs_total;
    bool flash_fs_mounted;
    /* TF 卡（容量可超 4GB → 64 位） */
    bool tf_mounted;
    uint64_t tf_used, tf_total;
    /* CPU */
    float cpu_temp; /* ℃；<0 = 不支持 */
    int cpu_load;   /* 双核平均 %（0~100，滚动窗口） */
    /* 系统 */
    uint32_t uptime_s;
    char chip[40];  /* "ESP32-S3 双核 240MHz" */
    char fw_ver[40];/* "v1.0.0"（放宽防 format-truncation） */
} ds_devstats_t;

bool ds_devstats_fetch(ds_devstats_t *out);

#endif /* DATA_SOURCE_H */
