package main

import (
	"crypto/rand"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// 阿里云【实时语音识别】(SpeechTranscriber) WebSocket 客户端。
//
// 和"一句话识别"(aliyun.go 的 Recognize)的区别：
//   - 一句话识别：HTTP POST 整段音频，整段录完才出结果，延时高。
//   - 实时识别：WebSocket 持续推流，阿里云【自动语义断句】，
//     边说边返回中间结果(ResultChanged)，一句说完返回 SentenceEnd。
//
// 协议要点（全程一个 WS 连接）：
//   1. 连 wss://nls-gateway-<region>.aliyuncs.com/ws/v1，带 header X-NLS-Token
//   2. 发 StartTranscription 指令(JSON 文本帧) -> 收 TranscriptionStarted
//   3. 持续发音频(二进制帧) -> 持续收 TranscriptionResultChanged / SentenceBegin / SentenceEnd
//   4. 发 StopTranscription -> 收 TranscriptionCompleted -> 关闭
//
// 指令/事件都是 JSON，结构：
//   {"header":{"namespace","name","message_id","task_id","appkey",...},"payload":{...}}

// nlsHeader 是阿里云 NLS 协议每条消息的 header。
type nlsHeader struct {
	MessageID  string `json:"message_id"`
	TaskID     string `json:"task_id"`
	Namespace  string `json:"namespace"`
	Name       string `json:"name"`
	Appkey     string `json:"appkey,omitempty"`
	Status     int    `json:"status,omitempty"`      // 事件里才有：20000000=成功
	StatusText string `json:"status_text,omitempty"` // 出错描述
}

// nlsMessage 是收发消息的通用外层结构。
type nlsMessage struct {
	Header  nlsHeader       `json:"header"`
	Payload json.RawMessage `json:"payload,omitempty"`
}

// transcriptionPayload 是 SentenceEnd / ResultChanged 事件里我们关心的字段。
type transcriptionPayload struct {
	Index  int    `json:"index"`  // 第几句
	Time   int    `json:"time"`   // 该句时长(ms)
	Result string `json:"result"` // 识别文字
}

// StreamCallback 在识别过程中被回调：
//   - onSentenceEnd：一句完整说完（带标点），这时去翻译。
//   - onPartial：中间结果（边说边出，未定稿），可选用于"正在听"提示。
type StreamCallback struct {
	onSentenceEnd func(text string)
	onPartial     func(text string)
}

// StreamRecognize 打开 WS、推送来自 audio 通道的 PCM、把识别事件回调出去。
// audio 通道关闭即表示音频结束（板子断开上传连接时关）。本函数阻塞直到识别结束。
func (a *AliyunASR) StreamRecognize(audio <-chan []byte, cb StreamCallback) error {
	token, err := a.getToken()
	if err != nil {
		return fmt.Errorf("取 Token 失败: %w", err)
	}

	wsURL := fmt.Sprintf("wss://nls-gateway-%s.aliyuncs.com/ws/v1", a.cfg.Region)
	dialer := websocket.Dialer{HandshakeTimeout: 10 * time.Second}
	header := http.Header{}
	header.Set("X-NLS-Token", token)

	conn, _, err := dialer.Dial(wsURL, header)
	if err != nil {
		return fmt.Errorf("连接阿里云实时识别 WS 失败: %w", err)
	}
	defer conn.Close()

	taskID := uuid32()

	// 1. 发 StartTranscription
	start := nlsMessage{
		Header: nlsHeader{
			MessageID: uuid32(),
			TaskID:    taskID,
			Namespace: "SpeechTranscriber",
			Name:      "StartTranscription",
			Appkey:    a.cfg.AppKey,
		},
		Payload: mustJSON(map[string]any{
			"format":                             "pcm",
			"sample_rate":                        16000,
			"enable_intermediate_result":         true, // 要中间结果(边说边出)
			"enable_punctuation_prediction":      true, // 加标点 -> 断句更准
			"enable_inverse_text_normalization":  true, // "一百二十" -> "120"
			"max_sentence_silence":               400,  // 句末静音 400ms 判一句结束。
			// 阿里云范围 200~2000ms。调小→断句更勤(连贯朗读也能在换气/逗号处切开，流式更快)，
			// 但太小会把一句话切碎；调大→更完整但连贯语流会几十秒不断句(见 Demo15 实测坑)。
		}),
	}
	if err := conn.WriteJSON(start); err != nil {
		return fmt.Errorf("发 StartTranscription 失败: %w", err)
	}

	// 2. 等 TranscriptionStarted（用一个标志位，读循环里置位）
	started := make(chan struct{})
	readErr := make(chan error, 1)

	// 读循环：解析事件、回调
	go func() {
		startedClosed := false
		for {
			mt, data, err := conn.ReadMessage()
			if err != nil {
				readErr <- err
				return
			}
			if mt != websocket.TextMessage {
				continue // 识别只回 JSON 文本帧
			}
			var msg nlsMessage
			if json.Unmarshal(data, &msg) != nil {
				continue
			}
			switch msg.Header.Name {
			case "TranscriptionStarted":
				if !startedClosed {
					close(started)
					startedClosed = true
				}
			case "TranscriptionResultChanged":
				if cb.onPartial != nil {
					var p transcriptionPayload
					if json.Unmarshal(msg.Payload, &p) == nil && p.Result != "" {
						cb.onPartial(p.Result)
					}
				}
			case "SentenceEnd":
				var p transcriptionPayload
				if json.Unmarshal(msg.Payload, &p) == nil && strings.TrimSpace(p.Result) != "" {
					cb.onSentenceEnd(p.Result)
				}
			case "TranscriptionCompleted":
				readErr <- nil // 正常结束
				return
			case "TaskFailed":
				readErr <- fmt.Errorf("阿里云任务失败: %s", msg.Header.StatusText)
				return
			}
		}
	}()

	// 等开始（或失败）
	select {
	case <-started:
	case err := <-readErr:
		if err != nil {
			return fmt.Errorf("等 TranscriptionStarted 出错: %w", err)
		}
		return nil
	case <-time.After(10 * time.Second):
		return fmt.Errorf("等 TranscriptionStarted 超时")
	}

	// 3. 持续把音频转成二进制帧推上去，直到 audio 通道关闭
	for chunk := range audio {
		if len(chunk) == 0 {
			continue
		}
		if err := conn.WriteMessage(websocket.BinaryMessage, chunk); err != nil {
			log.Printf("[ws] 推音频失败: %v", err)
			break
		}
	}

	// 4. 音频发完，发 StopTranscription，等阿里云把尾句识别完
	stop := nlsMessage{
		Header: nlsHeader{
			MessageID: uuid32(),
			TaskID:    taskID,
			Namespace: "SpeechTranscriber",
			Name:      "StopTranscription",
			Appkey:    a.cfg.AppKey,
		},
	}
	if err := conn.WriteJSON(stop); err != nil {
		log.Printf("[ws] 发 StopTranscription 失败: %v", err)
	}

	// 等读循环收到 TranscriptionCompleted（或出错）
	select {
	case err := <-readErr:
		if err != nil && !websocket.IsCloseError(err, websocket.CloseNormalClosure) {
			return err
		}
		return nil
	case <-time.After(10 * time.Second):
		return nil // 超时也算结束，尾句可能已回调
	}
}

// ---------------- 工具 ----------------

// uuid32 生成阿里云要求的 32 位十六进制字符串(无连字符的 UUID)。
func uuid32() string {
	b := make([]byte, 16)
	rand.Read(b)
	return fmt.Sprintf("%x", b)
}

func mustJSON(v any) json.RawMessage {
	b, _ := json.Marshal(v)
	return b
}
