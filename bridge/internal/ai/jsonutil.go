package ai

import (
	"bytes"
	"encoding/json"
	"io"
)

func bytesReader(b []byte) io.Reader { return bytes.NewReader(b) }

// jsonGetString 宽松取顶层字符串字段（body 非法 JSON 时返回空串）。
func jsonGetString(body []byte, key string) string {
	var m map[string]any
	if err := json.Unmarshal(body, &m); err != nil {
		return ""
	}
	s, _ := m[key].(string)
	return s
}

// jsonSetString 返回改写顶层字符串字段后的 JSON；非法 JSON 返回错误。
func jsonSetString(body []byte, key, value string) ([]byte, error) {
	var m map[string]any
	if err := json.Unmarshal(body, &m); err != nil {
		return nil, err
	}
	m[key] = value
	return json.Marshal(m)
}
