/* audio_mock — 模拟器音频 ops 装配入口。
 * 固件侧对应 audio_real.h（I2S+ES8311/ES7210，TODO Phase 6）。 */
#ifndef AUDIO_MOCK_H
#define AUDIO_MOCK_H

#include "voice_session.h"

const vs_audio_ops_t *audio_mock_ops(void);

#endif /* AUDIO_MOCK_H */
