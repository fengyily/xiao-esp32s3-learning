package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// DeepSeek 用 OpenAI 兼容的 /chat/completions 接口做对话。
type DeepSeek struct {
	cfg Config
}

func NewDeepSeek(cfg Config) *DeepSeek {
	return &DeepSeek{cfg: cfg}
}

// 请求体（OpenAI 兼容格式）
type chatMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}
type chatReq struct {
	Model    string        `json:"model"`
	Messages []chatMessage `json:"messages"`
	Stream   bool          `json:"stream"`
}

// 响应体（只取要用的字段）
type chatResp struct {
	Choices []struct {
		Message chatMessage `json:"message"`
	} `json:"choices"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

// Chat 把用户文字发给 DeepSeek，返回回答文字（语音助手人设）。
func (d *DeepSeek) Chat(userText string) (string, error) {
	return d.chatWithSystem("你是一个嵌入式语音助手，回答要简洁口语化，控制在两三句话以内。", userText)
}

// chatWithSystem 用指定的 system 提示调 DeepSeek，返回回答。Chat/Translate 共用。
func (d *DeepSeek) chatWithSystem(system, userText string) (string, error) {
	if d.cfg.DeepSeekKey == "" {
		return "", fmt.Errorf("未配置 DEEPSEEK_API_KEY")
	}

	body, _ := json.Marshal(chatReq{
		Model: d.cfg.DeepSeekModel,
		Messages: []chatMessage{
			{Role: "system", Content: system},
			{Role: "user", Content: userText},
		},
		Stream: false,
	})

	req, err := http.NewRequest(http.MethodPost,
		"https://api.deepseek.com/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+d.cfg.DeepSeekKey)
	req.Header.Set("Content-Type", "application/json")

	client := &http.Client{Timeout: 60 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", fmt.Errorf("请求 DeepSeek 失败: %w", err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)

	var r chatResp
	if err := json.Unmarshal(raw, &r); err != nil {
		return "", fmt.Errorf("解析 DeepSeek 响应失败: %w, body=%s", err, string(raw))
	}
	if r.Error != nil {
		return "", fmt.Errorf("DeepSeek 错误: %s", r.Error.Message)
	}
	if len(r.Choices) == 0 {
		return "", fmt.Errorf("DeepSeek 无回答, body=%s", string(raw))
	}
	return r.Choices[0].Message.Content, nil
}
