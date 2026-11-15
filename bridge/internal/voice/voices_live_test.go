// voices_live_test.go — Edge 免费通道中文音色目录回归（默认跳过；
// GADGET_VOICE_LIVE=1 时跑）。
//
// 2026-11-15 真网逐个探测 25 个候选音色：免费通道只认 Edge 朗读用的
// 子集，其余（Yunze/Xiaochen/Xiaohan/Xiaomeng/Xiaomo/Xiaoqiu/Xiaorou/
// Xiaorui/Xiaoshuang/Xiaoyan/Xiaoyou/Xiaozhen 共 12 个 Azure 目录音色）
// 返回 close 1007 "Unsupported voice"。下面 edgeCNVoiceGallery 就是
// 实测可用的全部中文音色，与 web/src/pages/Bots.vue 的下拉保持同步；
// 通道音色库变动时本测试先红，提醒同步 UI。
package voice

import (
	"context"
	"testing"
	"time"
)

// edgeCNVoiceGallery 免费通道实测可用的中文音色（zh-CN 7 + 东北/陕西
// 方言 2 + 粤语 2 + 台湾 2）。改这里要同步 Bots.vue。
var edgeCNVoiceGallery = []string{
	"zh-CN-XiaoxiaoNeural", "zh-CN-XiaoyiNeural", "zh-CN-XiaoxuanNeural",
	"zh-CN-YunjianNeural", "zh-CN-YunxiNeural", "zh-CN-YunxiaNeural",
	"zh-CN-YunyangNeural",
	"zh-CN-liaoning-XiaobeiNeural", "zh-CN-shaanxi-XiaoniNeural",
	"zh-HK-HiuMaanNeural", "zh-HK-WanLungNeural",
	"zh-TW-HsiaoChenNeural", "zh-TW-YunJheNeural",
}

func TestEdgeVoiceGalleryLive(t *testing.T) {
	liveEnabled(t)
	for _, v := range edgeCNVoiceGallery {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		pcm, err := edgeSynthesize(ctx, "你好呀。", v)
		cancel()
		if err != nil {
			t.Errorf("%s: %v", v, err)
			continue
		}
		t.Logf("%-32s ok %6d samples", v, len(pcm))
	}
}
