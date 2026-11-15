package voice

import (
	"strings"
	"testing"
	"unicode/utf8"
)

// 真实样本：AstrBot+DeepSeek 把推理英文混进回复（2026-11-15 真机）
const leaked = "User asks about Wuhan weather. I don't have real-time info. " +
	"Could use get_daily_news? No, that's news/热榜, not weather. Stay in " +
	"character: can't check. Keep it short, tsundere.武汉十一月中，白天大概十" +
	"度出头，早晚更凉，湿气重，阴雨天不少——这是常识，不是今天实况。具体今天" +
	"下不下雨，我查不到，你自己看预报或者开窗试试。要出门就带把伞、穿厚点。"

func TestSanitizeDropsEnglishPreamble(t *testing.T) {
	got := sanitizeVoiceReply(leaked)
	if strings.Contains(got, "User asks") || strings.Contains(got, "tsundere") {
		t.Fatalf("英文前言未清理: %q", got)
	}
	if !strings.HasPrefix(got, "武汉") {
		t.Fatalf("应以中文正文开头, got %q", got)
	}
	if n := utf8.RuneCountInString(got); n > maxVoiceReplyRunes {
		t.Fatalf("未截断: %d 字", n)
	}
	if !strings.HasSuffix(got, "。") {
		t.Fatalf("截断后应以句号结尾: %q", got)
	}
}

func TestSanitizeKeepsPureChinese(t *testing.T) {
	in := "现在下午三点，外面有点热。多喝水。"
	if got := sanitizeVoiceReply(in); got != in {
		t.Fatalf("纯中文不应改动: %q -> %q", in, got)
	}
}

func TestSanitizeKeepsPureEnglish(t *testing.T) {
	// 纯英文回复没有中文正文可分界：原样保留（不误删）
	in := "It is three o'clock now. Drink some water."
	if got := sanitizeVoiceReply(in); got != in {
		t.Fatalf("纯英文不应被裁剪: %q -> %q", in, got)
	}
}

func TestSanitizeStripsMarkdown(t *testing.T) {
	got := sanitizeVoiceReply("- **注意**：带伞。\n- 穿厚点。")
	if strings.ContainsAny(got, "*#-") {
		t.Fatalf("markdown 未清理: %q", got)
	}
}
