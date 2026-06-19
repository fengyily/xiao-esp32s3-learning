package main

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Synthesize 调阿里云【语音合成 TTS】REST 接口，把文字合成音频。
// 复用 AliyunASR 的 Token（同一个智能语音交互项目，appkey + X-NLS-Token 通用）。
// 返回 16kHz / 16bit / 单声道 PCM，正好和板子录音、I2S 播放的格式一致——
// 板子拿到直接 i2s_write 即可，不用在 MCU 上解码。
//
// 接口文档：POST https://nls-gateway-<region>.aliyuncs.com/stream/v1/tts
//   format=pcm、sample_rate=16000、voice=<发音人>、text=<要合成的文字>
func (a *AliyunASR) Synthesize(text string) ([]byte, error) {
	if strings.TrimSpace(text) == "" {
		return nil, fmt.Errorf("TTS 文本为空")
	}
	token, err := a.getToken()
	if err != nil {
		return nil, err
	}

	endpoint := fmt.Sprintf("https://nls-gateway-%s.aliyuncs.com/stream/v1/tts", a.cfg.Region)
	q := url.Values{}
	q.Set("appkey", a.cfg.AppKey)
	q.Set("token", token)
	q.Set("text", text)
	q.Set("format", "pcm")        // 裸 PCM，板子直接喂 I2S
	q.Set("sample_rate", "16000") // 与录音/播放统一
	q.Set("voice", "aixia")       // 发音人：aixia 自然女声；中英混读尚可
	q.Set("volume", "60")         // 0-100，留点余量避免削顶
	q.Set("speech_rate", "0")     // -500~500，0 为正常语速
	reqURL := endpoint + "?" + q.Encode()

	req, err := http.NewRequest(http.MethodGet, reqURL, nil)
	if err != nil {
		return nil, err
	}

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("请求 TTS 失败: %w", err)
	}
	defer resp.Body.Close()

	// 成功时返回音频流(Content-Type: audio/*)；失败时返回 JSON 错误。
	ct := resp.Header.Get("Content-Type")
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("读取 TTS 响应失败: %w", err)
	}
	if !strings.HasPrefix(ct, "audio") {
		return nil, fmt.Errorf("TTS 失败: content-type=%s body=%s", ct, string(body))
	}
	return body, nil
}
