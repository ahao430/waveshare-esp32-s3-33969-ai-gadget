
## 2026-11-18：本地音乐播放器 v1（WAV+MP3）

- 播放页（黑胶唱片对象绘制+光点绕行+进度弧+频谱+五键）/ 曲库列表 /
  后台常驻播放 + 状态栏药丸；`/sdcard/music/` 扫 MP3/WAV；ID3 v2.3/2.4
  + v1 标签（GBK 回退文件名）
- 音频输出 owner 仲裁（audio_esp32）：音乐持有期语音回复静音/提示音丢
  弃/PTT 拒绝提示；esp_codec_dev close+open 切采样率（无 set_fs）
- 坑 #26：新组件静态账——内部 RAM 大项（解码栈/曲库/FFT）全 PSRAM 惰性，
  解码任务栈 PSRAM（16KB）；sdkconfig 只留 MP3 解码器
- 坑 #27：PSRAM 池精确账（8MB−rodata−text），"4416K 老数"勘误为 2560K
- regress-sim 基线 8→9（音乐接入）；模拟器 headless 截帧闭环验证
- 固件 5.60MiB/47%；post-ui internal 64.7KB（>56KB 底线）；esp_audio_codec
  2.6.2 + esp-dsp 1.8.2 组件入库
