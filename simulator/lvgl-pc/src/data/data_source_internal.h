/* data_source 内部协议：front 与各实现共享，不暴露给 UI。 */
#ifndef DATA_SOURCE_INTERNAL_H
#define DATA_SOURCE_INTERNAL_H

#include <stdbool.h>

#include "data_source.h"

typedef struct {
	const char *name;                     /* 人读名称（静态字符串） */
	void (*init)(void);                   /* 可为 NULL */
	bool (*refresh)(gadget_state_t *st);  /* true=成功 */
	/* 智谱预热（PLAN §5.6）：实现负责调用 Bridge/本地模拟，结果写 msg。
	 * 返回值语义见 data_source.h 的 ds_prewarm_result_t。 */
	ds_prewarm_result_t (*prewarm_zhipu)(gadget_state_t *st, char *msg, int msg_len);
	/* Bigfish Chat 一轮流式对话（见 data_source.h）。 */
	ds_chat_result_t (*chat_send_stream)(const ds_chat_msg_t *msgs, int n,
	                                     ds_chat_delta_cb_t on_delta, void *user,
	                                     char *reply_full, int reply_len);
	/* 语音一步式交互（见 data_source.h）。 */
	ds_voice_result_t (*voice_interact)(const int16_t *pcm, int frames,
	                                    char *transcript, int transcript_len,
	                                    char *reply, int reply_len);
	/* 聊天 provider 列表（"\\n" 分隔；可为 NULL，front 兜底 "mock"）。 */
	int (*chat_providers)(char *out, int cap);
	/* 自定义背景拉取（可为 NULL=不支持）：成功把图片字节写入 save_path。
	 * 返回 0=已写入；1=无背景（HTTP 404）；-1=失败。 */
	int (*fetch_background)(const char *save_path);
	/* Web 下发主题拉取（可为 NULL=不支持）：0=拿到 *theme / 1=默认 / -1 */
	int (*fetch_theme)(int *theme);
	/* Web 下发按键映射拉取（可为 NULL=不支持）：0=四槽写入（每槽 ≥8 字节）
	 * / 1=未配置默认（不写） / -1=失败。槽值域见 /api/v1/keys。 */
	int (*fetch_keys)(char *pwr_short, char *boot_short,
	                  char *key3_short, char *key3_long, int cap);

} ds_impl_t;

#endif /* DATA_SOURCE_INTERNAL_H */
