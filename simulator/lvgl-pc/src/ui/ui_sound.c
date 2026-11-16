/* ui_sound — 实现，见 ui_sound.h。 */
#include "ui_sound.h"

#ifdef ESP_PLATFORM
#include "voice/audio_esp32.h"

void ui_sound_click(void) { audio_esp32_ui_beep(AUDIO_BEEP_CLICK); }
void ui_sound_alert(void) { audio_esp32_ui_beep(AUDIO_BEEP_ALERT); }

#else
#include <stdio.h>

/* 模拟器无声卡路径：打日志供 GADGET_SNAP 回归断言触发时机 */
void ui_sound_click(void) { fprintf(stderr, "[sound] click\n"); }
void ui_sound_alert(void) { fprintf(stderr, "[sound] alert\n"); }
#endif
