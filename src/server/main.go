// XIAO ESP32-S3 对话功能 —— Go 中转服务（第一版：只做 ASR）
//
// 板子录音 PCM(16k/16bit/单声道) --POST /asr--> 本服务 --> 阿里云一句话识别 --> 返回文字
//
// 运行前设置环境变量（见 .env.example）：
//   ALIYUN_AK_ID / ALIYUN_AK_SECRET / ALIYUN_APPKEY
// 启动：
//   cd src/server && go run .
package main

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"time"
)

func main() {
	cfg := loadConfig()
	asr := NewAliyunASR(cfg)
	ds := NewDeepSeek(cfg)

	mux := http.NewServeMux()

	// 健康检查：板子/浏览器可先访问这个确认能连通
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte("ok"))
	})

	// /asr：请求体是原始 PCM 音频字节，返回 {"text":"识别结果"}
	mux.HandleFunc("/asr", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "use POST", http.StatusMethodNotAllowed)
			return
		}
		start := time.Now()

		pcm, err := io.ReadAll(r.Body)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "读取音频失败: " + err.Error()})
			return
		}
		log.Printf("/asr 收到音频 %d 字节", len(pcm))
		pcm = resampleFromRequest(r, pcm)
		if p, err := saveWAV(pcm); err == nil {
			log.Printf("已保存录音(重采样后): %s", p)
		}
		if len(pcm) == 0 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "音频为空"})
			return
		}

		text, err := asr.Recognize(pcm)
		if err != nil {
			log.Printf("识别出错: %v", err)
			writeJSON(w, http.StatusBadGateway, map[string]string{"error": err.Error()})
			return
		}
		log.Printf("识别结果(%.1fs): %q", time.Since(start).Seconds(), text)
		writeJSON(w, http.StatusOK, map[string]string{"text": text})
	})

	// /chat：请求体是 PCM 音频 → 识别 → DeepSeek 对话 → 返回 {"text":识别的,"reply":回答}
	mux.HandleFunc("/chat", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "use POST", http.StatusMethodNotAllowed)
			return
		}
		start := time.Now()

		pcm, err := io.ReadAll(r.Body)
		if err != nil || len(pcm) == 0 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "音频为空"})
			return
		}
		log.Printf("/chat 收到音频 %d 字节", len(pcm))
		// 板子 PDM 实际采样率≈17469，用 query 参数 src_rate 告知；重采样到 16000 再识别
		pcm = resampleFromRequest(r, pcm)
		// 存成 WAV 留档，方便检查音质（排查识别不准）
		if p, err := saveWAV(pcm); err == nil {
			log.Printf("已保存录音(重采样后): %s", p)
		}

		// 1. 识别
		text, err := asr.Recognize(pcm)
		if err != nil {
			log.Printf("识别出错: %v", err)
			writeJSON(w, http.StatusBadGateway, map[string]string{"error": "识别失败: " + err.Error()})
			return
		}
		log.Printf("识别: %q", text)
		if text == "" {
			writeJSON(w, http.StatusOK, map[string]string{"text": "", "reply": "（没听清，请再说一次）"})
			return
		}

		// 2. DeepSeek 对话
		reply, err := ds.Chat(text)
		if err != nil {
			log.Printf("对话出错: %v", err)
			// 识别成功但对话失败：把识别结果也返回，便于排查
			writeJSON(w, http.StatusBadGateway, map[string]string{"text": text, "error": "对话失败: " + err.Error()})
			return
		}
		log.Printf("回答(%.1fs): %q", time.Since(start).Seconds(), reply)
		writeJSON(w, http.StatusOK, map[string]string{"text": text, "reply": reply})
	})

	// /translate：PCM 音频 → 识别 → 自动判方向翻译(中↔英) → {"text":原文,"translation":译文}
	mux.HandleFunc("/translate", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "use POST", http.StatusMethodNotAllowed)
			return
		}
		start := time.Now()
		pcm, err := io.ReadAll(r.Body)
		if err != nil || len(pcm) == 0 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "音频为空"})
			return
		}
		pcm = resampleFromRequest(r, pcm)
		if p, err := saveWAV(pcm); err == nil {
			log.Printf("已保存录音: %s", p)
		}

		text, err := asr.Recognize(pcm)
		if err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]string{"error": "识别失败: " + err.Error()})
			return
		}
		log.Printf("识别: %q", text)
		if text == "" {
			writeJSON(w, http.StatusOK, map[string]string{"text": "", "translation": "（没听清，请再说一次）"})
			return
		}

		trans, target, err := ds.Translate(text)
		if err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]string{"text": text, "error": err.Error()})
			return
		}
		log.Printf("译文(%s, %.1fs): %q", target, time.Since(start).Seconds(), trans)
		writeJSON(w, http.StatusOK, map[string]string{"text": text, "translation": trans})
	})

	log.Printf("中转服务启动，监听 %s（POST 音频到 /asr）", cfg.Addr)
	if err := http.ListenAndServe(cfg.Addr, mux); err != nil {
		log.Fatal(err)
	}
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}
