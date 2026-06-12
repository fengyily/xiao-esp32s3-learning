package main

import (
	"log"
	"os"
)

// Config 是中转服务的运行配置。
// 所有密钥从环境变量读取，绝不写死在代码里（也不进 git）。
type Config struct {
	AccessKeyID     string // 阿里云 AccessKey ID
	AccessKeySecret string // 阿里云 AccessKey Secret
	AppKey          string // 智能语音交互项目的 Appkey
	Region          string // 如 cn-shanghai
	Addr            string // 服务监听地址，如 :8080

	DeepSeekKey   string // DeepSeek API Key
	DeepSeekModel string // 模型名，默认 deepseek-chat
}

// loadConfig 读取环境变量，缺关键项就直接退出（fail fast）。
func loadConfig() Config {
	cfg := Config{
		AccessKeyID:     os.Getenv("ALIYUN_AK_ID"),
		AccessKeySecret: os.Getenv("ALIYUN_AK_SECRET"),
		AppKey:          os.Getenv("ALIYUN_APPKEY"),
		Region:          getenvDefault("ALIYUN_REGION", "cn-shanghai"),
		Addr:            getenvDefault("LISTEN_ADDR", ":8080"),
		DeepSeekKey:     os.Getenv("DEEPSEEK_API_KEY"),
		DeepSeekModel:   getenvDefault("DEEPSEEK_MODEL", "deepseek-chat"),
	}

	missing := ""
	if cfg.AccessKeyID == "" {
		missing += " ALIYUN_AK_ID"
	}
	if cfg.AccessKeySecret == "" {
		missing += " ALIYUN_AK_SECRET"
	}
	if cfg.AppKey == "" {
		missing += " ALIYUN_APPKEY"
	}
	if missing != "" {
		log.Fatalf("缺少必需的环境变量:%s（参考 .env.example）", missing)
	}
	return cfg
}

func getenvDefault(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
