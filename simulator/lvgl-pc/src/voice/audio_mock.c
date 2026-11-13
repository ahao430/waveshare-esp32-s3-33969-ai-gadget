/* audio_mock — vs_audio_ops_t 的模拟器实现（PLAN §11.4 mock 硬件层）。
 * 采集=置位标志（假帧由 vs_tick 喂 listener）；播放=立即回调完成。
 * 固件 real adapter（I2S+ES8311/ES7210）TODO，见 docs/HARDWARE.md §2.4。
 */
#include "voice_session.h"

#include <stdbool.h>

static bool g_capturing;

static int mock_start_capture(void)
{
    g_capturing = true;
    return 0;
}

static void mock_stop_capture(void)
{
    g_capturing = false;
}

static int mock_play(const int16_t *pcm, int frames)
{
    (void)pcm;
    (void)frames;
    /* 非阻塞"播放"：立即完成，下一 tick 通知。 */
    return 0;
}

static void mock_cancel_play(void)
{
    /* no-op */
}

static const vs_audio_ops_t g_mock_ops = {
    .start_capture = mock_start_capture,
    .stop_capture = mock_stop_capture,
    .play = mock_play,
    .cancel_play = mock_cancel_play,
};

/* 暴露给 main/ui 装配（固件侧替换为 audio_real_ops()）。 */
const vs_audio_ops_t *audio_mock_ops(void);
const vs_audio_ops_t *audio_mock_ops(void)
{
    return &g_mock_ops;
}
