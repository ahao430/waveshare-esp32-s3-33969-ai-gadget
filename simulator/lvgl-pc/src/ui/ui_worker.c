/* ui_worker — 见 ui_worker.h。 */
#include "ui_worker.h"

#include <pthread.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#define UI_WORKER_MIN_STACK (12 * 1024)

int ui_worker_start(void *(*fn)(void *), void *arg, size_t stack_size)
{
    if (!fn) return -1;
    if (stack_size < UI_WORKER_MIN_STACK) stack_size = UI_WORKER_MIN_STACK;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    int rc = pthread_create(&tid, &attr, fn, arg);

    if (rc != 0 && stack_size > UI_WORKER_MIN_STACK) {
#ifdef ESP_PLATFORM
        /* 内部 RAM 碎片化（largest < 请求值）：回落 12KB 再试一次。
         * 每次启动只告警一次，避免每次进页面刷屏。 */
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW("worker", "pthread_create(%u) rc=%d; retry 12K (once)",
                     (unsigned)stack_size, rc);
        }
#endif
        pthread_attr_setstacksize(&attr, UI_WORKER_MIN_STACK);
        rc = pthread_create(&tid, &attr, fn, arg);
    }
    if (rc == 0) pthread_detach(tid);
    pthread_attr_destroy(&attr);
    return rc;
}