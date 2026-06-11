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
