# 对话功能 Go 中转服务（第一版：ASR）

板子录音 → POST 给本服务 → 本服务调阿里云一句话识别 → 返回识别文字。
密钥留在服务端，板子不接触阿里云签名。详见 [../../DIALOGUE_PLAN.md](../../DIALOGUE_PLAN.md)。

## 前置：阿里云开通

1. 开通 [智能语音交互](https://nls-portal.console.aliyun.com)，创建一个**项目**，拿到 **Appkey**。
2. 在 RAM 创建一个**子用户**，只授予语音交互权限，生成 **AccessKey ID / Secret**（最小权限，降低泄露风险）。

## 配置 & 运行

```bash
cd src/server
cp .env.example .env      # 填入真实密钥
source .env
go run .                  # 监听 :8080
```

## 接口

| 方法 | 路径 | 请求体 | 返回 |
|---|---|---|---|
| GET | `/health` | — | `ok` |
| POST | `/asr` | 原始 PCM 音频(16kHz/16bit/单声道) | `{"text":"识别结果"}` 或 `{"error":"..."}` |

## 本地自测（不用板子，用一个 PCM 文件）

```bash
# 假设有一个 16k/16bit/单声道的 test.pcm
curl -X POST --data-binary @test.pcm http://localhost:8080/asr
```

## 后续

- [ ] 加 `/chat`：ASR 结果发给 Claude，返回回答文字
- [ ] 加 TTS：回答合成语音返回（配合板子外接 MAX98357A 喇叭）
