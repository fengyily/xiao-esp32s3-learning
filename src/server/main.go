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

	// /translate_ws：板子用裸 WiFiClient 持续 POST PCM（不切片，连接保持打开）。
	//   Go 把音频流转推阿里云【实时识别】WS，由阿里云自动语义断句；
	//   每收到一句完整文字(SentenceEnd) → DeepSeek 流式翻译 → 立刻写回板子。
	//   行协议同 /translate_stream：T:原文\n  多行 D:译文增量\n  每句末 E\n
	//
	// 这是 Demo 15 的核心：用专业流式 ASR 在【语义停顿】处断句，
	// 取代固定时长切片(会把单词切碎)，延时低且不丢字。
	mux.HandleFunc("/translate_ws", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "use POST", http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		flusher, _ := w.(http.Flusher)

		// 串行写回板子：阿里云断句回调可能并发触发翻译，统一交给一个 writer 顺序处理，
		// 既不阻塞音频推流，也避免多 goroutine 同时写 response。
		type sentence struct{ text string }
		sentCh := make(chan sentence, 16)

		// 把板子上传的 body 流切成块喂给 WS 推流协程
		audioCh := make(chan []byte, 32)
		go func() {
			defer close(audioCh)
			buf := make([]byte, 3200) // 16k/16bit -> 100ms 一块
			for {
				n, err := r.Body.Read(buf)
				if n > 0 {
					chunk := make([]byte, n)
					copy(chunk, buf[:n])
					audioCh <- chunk
				}
				if err != nil {
					return // 板子断开上传 = 音频结束
				}
			}
		}()

		// 翻译+写回协程（串行，顺序输出）
		writeDone := make(chan struct{})
		go func() {
			defer close(writeDone)
			writeLine := func(s string) {
				w.Write([]byte(s))
				if flusher != nil {
					flusher.Flush()
				}
			}
			for s := range sentCh {
				text := s.text
				writeLine("T:" + text + "\n")

				sys := "你是翻译引擎。把用户输入翻译成地道自然的英文。只输出英文译文，不要解释、引号、拼音。"
				if !hasChinese(text) {
					sys = "你是翻译引擎。把用户输入翻译成地道自然的简体中文。只输出中文译文，不要解释、引号。"
				}
				tStart := time.Now()
				firstDelta := time.Duration(-1)
				err := ds.streamWithSystem(sys, text, func(delta string) {
					if firstDelta < 0 {
						firstDelta = time.Since(tStart)
					}
					delta = strings.ReplaceAll(delta, "\n", " ")
					writeLine("D:" + delta + "\n")
				})
				if err != nil {
					writeLine("D:[翻译出错]\n")
				}
				writeLine("E\n")
				log.Printf("[ws] 句子 %q | 翻译首字 %dms 整句 %dms",
					text, firstDelta.Milliseconds(), time.Since(tStart).Milliseconds())
			}
		}()

		// 阻塞跑识别：SentenceEnd 时把整句丢进 sentCh
		err := asr.StreamRecognize(audioCh, StreamCallback{
			onSentenceEnd: func(text string) {
				select {
				case sentCh <- sentence{text}:
				default:
					log.Printf("[ws] 句子队列满，丢弃: %q", text)
				}
			},
		})
		close(sentCh)
		<-writeDone // 等最后一句翻完写完
		if err != nil {
			log.Printf("[ws] 识别结束(带错误): %v", err)
		}
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
