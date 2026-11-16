/* ui_worker — 统一的 pthread worker 创建（内部 RAM 栈）。
 *
 * 真机内部 RAM 碎片化时 largest block 可能 <16KB：16KB 栈的
 * pthread_create 直接 ENOMEM（rc=12，2026-11-16 实测订阅/天气 worker
 * 静默不启动 → 订阅页永远空）。策略：按请求大小创建；ENOMEM 且请求
 * >12KB 时回落 12KB 重试（12KB 是 boot 后 largest_int≈13.8KB 内能
 * 稳定拿到的上限）。调用方拿到 rc≠0 时按原有失败路径处理。
 */
#ifndef UI_WORKER_H
#define UI_WORKER_H

#include <stddef.h>

/* 创建 detached worker；成功返回 0，失败返回 pthread 错误码。 */
int ui_worker_start(void *(*fn)(void *), void *arg, size_t stack_size);

#endif /* UI_WORKER_H */