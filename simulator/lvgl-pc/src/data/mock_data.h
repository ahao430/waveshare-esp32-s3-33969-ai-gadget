/* mock_data — data_source mock 实现的底层伪随机数据。
 * UI 不直接 include 本文件（一律走 data_source.h）。
 */
#ifndef MOCK_DATA_H
#define MOCK_DATA_H

typedef struct {
    /* 额度（%） */
    int zhipu_used_5h;
    int zhipu_used_week;
    long zhipu_reset_5h;   /* unix 秒 */
    long zhipu_reset_week; /* unix 秒 */
    char zhipu_plan[48];

    int kimi_used_5h;
    int kimi_used_week;

    /* 系统状态 */
    int battery_pct;
    int wifi_ok; /* 0/1 */
    long updated_unix;
} mock_state_t;

/* 全局只读状态；refresh 后立即生效 */
const mock_state_t *mock_state(void);

/* 模拟一次手动刷新：伪随机更新额度数值 */
void mock_refresh(void);

#endif /* MOCK_DATA_H */
