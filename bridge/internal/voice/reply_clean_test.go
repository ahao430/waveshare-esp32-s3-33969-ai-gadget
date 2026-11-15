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

func TestStripParenNotes(t *testing.T) {
	cases := map[string]string{
		"（笑着）今天有点冷。":              "今天有点冷。",
		"带伞（认真）穿厚点。":              "带伞穿厚点。",
		"半角也要去(thinking)掉。":         "半角也要去掉。",
		"落单右括号)保留。":                 "落单右括号)保留。",
		"没有括号的句子。":                  "没有括号的句子。",
	}
	for in, want := range cases {
		if got := stripParenNotes(in); got != want {
			t.Errorf("stripParenNotes(%q)=%q want %q", in, got, want)
		}
	}
}

func TestTtsSpeakTextSkipsReasoningAndParens(t *testing.T) {
	// 推理句（英文长句）：不念
	if got := ttsSpeakText("User asks again, note repetition with slight irritation."); got != "" {
		t.Fatalf("reasoning should be dropped, got %q", got)
	}
	// 中文+括号描写：念正文去括号
	if got := ttsSpeakText("（害羞地笑）今天带你吃火锅好不好？"); got != "今天带你吃火锅好不好？" {
		t.Fatalf("paren note should be stripped, got %q", got)
	}
	// 带英文品牌名的正常中文句：照念
	if got := ttsSpeakText("用 DeepSeek 查的，准没错。"); got == "" {
		t.Fatalf("normal sentence with brand dropped")
	}
}

func TestSpeechValid(t *testing.T) {
	valid := []string{"今天天气怎么样", "hello", "3点开会", "嗯嗯好的"}
	invalid := []string{"", "   ", "。", "…", "，？！"}
	for _, s := range valid {
		if !speechValid(s) {
			t.Errorf("speechValid(%q) should be true", s)
		}
	}
	for _, s := range invalid {
		if speechValid(s) {
			t.Errorf("speechValid(%q) should be false", s)
		}
	}
}
