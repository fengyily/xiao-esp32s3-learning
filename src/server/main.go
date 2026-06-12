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
	"strings"
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

	// /chat_text：调试用，直接传文本对话（带会话上下文），不走录音。
	//   GET /chat_text?text=你好
	mux.HandleFunc("/chat_text", func(w http.ResponseWriter, r *http.Request) {
		text := r.URL.Query().Get("text")
		if text == "" {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "缺 text 参数"})
			return
		}
		reply, err := ds.Chat(text)
		if err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]string{"error": err.Error()})
			return
		}
		log.Printf("[chat_text] %q -> %q", text, reply)
		writeJSON(w, http.StatusOK, map[string]string{"text": text, "reply": reply})
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

	// /translate_stream：识别 → DeepSeek 流式翻译，边译边吐给板子（打字机效果）。
	// 行协议：T:原文\n  然后多行 D:译文增量\n  最后 E\n
	mux.HandleFunc("/translate_stream", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "use POST", http.StatusMethodNotAllowed)
			return
		}
		pcm, err := io.ReadAll(r.Body)
		if err != nil || len(pcm) == 0 {
			http.Error(w, "音频为空", http.StatusBadRequest)
			return
		}
		pcm = resampleFromRequest(r, pcm)

		text, err := asr.Recognize(pcm)
		if err != nil {
			http.Error(w, "识别失败: "+err.Error(), http.StatusBadGateway)
			return
		}
		log.Printf("[stream] 识别: %q", text)

		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		flusher, _ := w.(http.Flusher)
		writeLine := func(s string) {
			w.Write([]byte(s))
			if flusher != nil {
				flusher.Flush() // 立刻发出去，不攒缓冲
			}
		}

		writeLine("T:" + text + "\n")
		if text == "" {
			writeLine("E\n")
			return
		}

		// 翻译方向（复用 translate.go 的判断），流式输出
		sys := "你是翻译引擎。把用户输入翻译成地道自然的英文。只输出英文译文，不要解释、引号、拼音。"
		if !hasChinese(text) {
			sys = "你是翻译引擎。把用户输入翻译成地道自然的简体中文。只输出中文译文，不要解释、引号。"
		}
		err = ds.streamWithSystem(sys, text, func(delta string) {
			// 增量里可能含换行，替换掉以免破坏行协议
			delta = strings.ReplaceAll(delta, "\n", " ")
			writeLine("D:" + delta + "\n")
		})
		if err != nil {
			writeLine("D:[翻译出错]\n")
		}
		writeLine("E\n")
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
