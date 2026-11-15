// reply_clean.go — 语音回复清洗：只留下"该被朗读"的中文正文。
//
// 真机实测（2026-11-15）：AstrBot + DeepSeek 的回复里常带一段模型推理
// 英文元叙述，例如：
//
//	User asks about Wuhan weather. I don't have real-time info. Could use
//	get_daily_news? No, that's news/热榜... Keep it short, tsundere.武汉十
//	一月中，白天大概十度出头……
//
// 这段被原样念出来：回复 362 字 → TTS 7.8s、播放 40s+，用户只感到"慢+
// 莫名其妙"。本文件做两件事（只作用于语音链路，不动文字聊天/历史展示）：
//  1. 丢掉中文正文之前的英文前言（推理/元指令），保留中文正文
//  2. 按句截断到 maxVoiceReplyRunes：语音场景一次念 2~3 句就够
//
// 不做"猜模型意图"的复杂清洗：只用"尾部中文占比"找正文起点 + 句读截断。
package voice

import (
	"strings"
	"unicode"
)

// maxVoiceReplyRunes：送给 TTS 的中文正文上限（约 3 句）。
// ~110 字 ≈ DeepSeek TTS 20 秒音频；再长就是在念文章了。
const maxVoiceReplyRunes = 110

// sanitizeVoiceReply 清洗语音回复：去英文前言 / markdown / 截断。
func sanitizeVoiceReply(s string) string {
	s = stripMarkdown(strings.TrimSpace(s))
	if s == "" {
		return s
	}
	if i := chineseBodyStart(s); i > 0 {
		s = strings.TrimSpace(s[i:])
	}
	return truncateSentences(s, maxVoiceReplyRunes)
}

// stripMarkdown 去掉语音不该念的符号与列表前缀，并把换行并成一句。
func stripMarkdown(s string) string {
	repl := strings.NewReplacer("*", "", "#", "", "`", "", "_", " ", "~", "")
	s = repl.Replace(s)
	lines := strings.Split(s, "\n")
	out := make([]string, 0, len(lines))
	for _, ln := range lines {
		ln = strings.TrimSpace(strings.TrimLeft(strings.TrimSpace(ln), "-+>• "))
		if ln != "" {
			out = append(out, ln)
		}
	}
	return strings.Join(out, "")
}

// chineseBodyStart 找中文正文起点（字节偏移）：
// 找第一段"连续 20 字里 ≥70% 是汉字/中文标点"的窗口——英文前缀里零星
// 出现的中文（如 "news/热榜"）凑不满窗口；同时要求前缀确实是英文为主
// （≥10 个 ASCII 字母），避免误删"OK，……"这类正常开头。
func chineseBodyStart(s string) int {
	runes := []rune(s)
	const win = 20
	for i := 0; i+win <= len(runes); i++ {
		han := 0
		for j := i; j < i+win; j++ {
			if isHanOrCJKPunct(runes[j]) {
				han++
			}
		}
		if han*10 < win*7 {
			continue
		}
		if i == 0 {
			return 0
		}
		letters := 0
		for j := 0; j < i; j++ {
			if (runes[j] >= 'a' && runes[j] <= 'z') || (runes[j] >= 'A' && runes[j] <= 'Z') {
				letters++
			}
		}
		if letters < 10 {
			return 0 /* 前缀不是英文叙述：不裁 */
		}
		/* 窗口起点可能落在英文单词尾巴（"…ndere.武汉"）：对齐到正文首字 */
		j := i
		for j < len(runes) && !isHanOrCJKPunct(runes[j]) {
			j++
		}
		return len(string(runes[:j]))
	}
	return 0
}

func isHanOrCJKPunct(r rune) bool {
	if unicode.Is(unicode.Han, r) {
		return true
	}
	switch r {
	case '，', '。', '！', '？', '、', '；', '：', '“', '”', '（', '）':
		return true
	}
	return false
}

// ---- 语音播报专用清洗（文字展示不用；用户 2026-11-15 反馈）----
// 1. 括号内舞台指示（（笑着）/(thinking) 等）只上屏不朗读
// 2. 句内英文占比过高且无中文 = 模型推理/元叙述，整句不朗读
// 3. markdown 符号不朗读

// speechValid：ASR 结果里有没有实际内容（字母/汉字/数字）。
// 只有空白/标点（"。"、"嗯…"被拦成空也常见）视为没听清。
func speechValid(text string) bool {
	for _, r := range strings.TrimSpace(text) {
		if unicode.IsLetter(r) || unicode.IsDigit(r) {
			return true
		}
	}
	return false
}

// stripParenNotes 去掉成对括号及其内容（全角/半角，一层）。
// 落单的右括号保留（正文里偶尔出现）。
func stripParenNotes(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	depth := 0
	for _, r := range s {
		switch r {
		case '（', '(':
			depth++
		case '）', ')':
			if depth > 0 {
				depth--
			} else {
				b.WriteRune(r)
			}
		default:
			if depth == 0 {
				b.WriteRune(r)
			}
		}
	}
	return b.String()
}

// isLikelyReasoning：句内 ≥12 个 ASCII 字母且汉字/中文标点占比 <30%
// ——中文人设的正常回复句占比远高于此；英文推理句恰好相反。
func isLikelyReasoning(s string) bool {
	runes := []rune(s)
	han, letters := 0, 0
	for _, r := range runes {
		switch {
		case isHanOrCJKPunct(r):
			han++
		case (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z'):
			letters++
		}
	}
	return letters >= 12 && han*10 < len(runes)*3
}

// stripAsteriskNotes 去掉成对星号及其内容（*她笑了笑* 这类 RP 舞台
// 指示，与括号同语义：只上屏不朗读）。必须在 stripMarkdown 之前跑——
// 后者把星号删掉后就只剩纯文本动作描写，无从辨认了。
func stripAsteriskNotes(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	depth := 0
	for _, r := range s {
		switch r {
		case '*':
			if depth > 0 {
				depth--
			} else {
				depth++
			}
		default:
			if depth == 0 {
				b.WriteRune(r)
			}
		}
	}
	return b.String()
}

// ttsSpeakText 单句 → 朗读文本；空串 = 这句不念。
func ttsSpeakText(s string) string {
	s = stripParenNotes(s)
	s = stripAsteriskNotes(s)
	s = stripMarkdown(s)
	s = strings.TrimSpace(s)
	if s == "" || isLikelyReasoning(s) {
		return ""
	}
	return truncateSentences(s, maxVoiceReplyRunes)
}

// truncateSentences 按句截断到 limit 个字（rune），尽量落在句末标点后。
func truncateSentences(s string, limit int) string {
	runes := []rune(s)
	if len(runes) <= limit {
		return s
	}
	cut := limit
	for i := limit - 1; i >= limit/2; i-- {
		if isHanOrCJKPunct(runes[i]) || runes[i] == '.' || runes[i] == '!' || runes[i] == '?' {
			cut = i + 1
			break
		}
	}
	out := strings.TrimSpace(string(runes[:cut]))
	if out != "" && !strings.ContainsAny(out[len(out)-3:], "。！？") {
		out += "。"
	}
	return out
}
