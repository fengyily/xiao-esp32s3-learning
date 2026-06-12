package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
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

// Chat 带会话上下文对话：把历史+本句一起发 DeepSeek，回答存回历史。
// 这样多轮能连贯（"我想去"…"钓鱼" 后续能接上）。
func (d *DeepSeek) Chat(userText string) (string, error) {
	const system = "你是一个嵌入式语音助手，回答要简洁口语化，控制在两三句话以内。" +
		"用户是分句说话的，可能上一句没说完就停顿，请结合前文理解他的完整意图。"
	msgs := globalSession.buildMessages(system, userText)
	reply, err := d.chatWithMessages(msgs)
	if err != nil {
		return "", err
	}
	globalSession.commit(userText, reply)
	return reply, nil
}

// chatWithSystem 用指定 system + 单条 user 调 DeepSeek（无历史）。
func (d *DeepSeek) chatWithSystem(system, userText string) (string, error) {
	return d.chatWithMessages([]chatMessage{
		{Role: "system", Content: system},
		{Role: "user", Content: userText},
	})
}

// chatWithMessages 用完整消息数组（可含历史）调 DeepSeek，返回回答。
func (d *DeepSeek) chatWithMessages(messages []chatMessage) (string, error) {
	if d.cfg.DeepSeekKey == "" {
		return "", fmt.Errorf("未配置 DEEPSEEK_API_KEY")
	}

	body, _ := json.Marshal(chatReq{
		Model:    d.cfg.DeepSeekModel,
		Messages: messages,
		Stream:   false,
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

// streamWithSystem 流式调用 DeepSeek：每收到一段增量文本就调 onDelta。
// 用于"打字机式"输出——译文边生成边吐给板子，首字更快。
func (d *DeepSeek) streamWithSystem(system, userText string, onDelta func(string)) error {
	if d.cfg.DeepSeekKey == "" {
		return fmt.Errorf("未配置 DEEPSEEK_API_KEY")
	}
	body, _ := json.Marshal(chatReq{
		Model: d.cfg.DeepSeekModel,
		Messages: []chatMessage{
			{Role: "system", Content: system},
			{Role: "user", Content: userText},
		},
		Stream: true, // 关键：开流式
	})
	req, err := http.NewRequest(http.MethodPost,
		"https://api.deepseek.com/chat/completions", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+d.cfg.DeepSeekKey)
	req.Header.Set("Content-Type", "application/json")

	client := &http.Client{Timeout: 60 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("请求 DeepSeek 失败: %w", err)
	}
	defer resp.Body.Close()

	// DeepSeek 流式是 SSE：每行 "data: {json}"，最后 "data: [DONE]"
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		data := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if data == "[DONE]" {
			break
		}
		var chunk struct {
			Choices []struct {
				Delta struct {
					Content string `json:"content"`
				} `json:"delta"`
			} `json:"choices"`
		}
		if json.Unmarshal([]byte(data), &chunk) != nil {
			continue
		}
		if len(chunk.Choices) > 0 && chunk.Choices[0].Delta.Content != "" {
			onDelta(chunk.Choices[0].Delta.Content)
		}
	}
	return sc.Err()
}
