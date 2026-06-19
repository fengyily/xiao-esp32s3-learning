# 语音助手 Go 中转服务

板子只管录音/上传，复杂活全在这里：阿里云签名鉴权、ASR 识别、DeepSeek 对话/翻译。
密钥留在服务端，板子不接触。整体架构见 [../../DIALOGUE_PLAN.md](../../DIALOGUE_PLAN.md)。

```text
[板子] --PCM音频--> [本服务] --签名--> 阿里云 ASR --文字--> DeepSeek --回答/译文--> [板子]
```

## 前置：云端开通

1. **阿里云智能语音交互**：开通 [控制台](https://nls-portal.console.aliyun.com)，创建项目拿 **Appkey**；
   在 RAM 建子用户授予语音交互权限，生成 **AccessKey ID / Secret**（最小权限）。
2. **DeepSeek**：在 [platform.deepseek.com](https://platform.deepseek.com) 拿 **API Key**。

## 配置 & 运行

```bash
cd src/server
cp .env.example .env      # 填入真实密钥（.env 已 gitignore）
set -a && source .env && set +a
go run .                  # 默认监听 :8080
```

> 代码不自动读 .env，要先 `source` 进环境变量再跑。

## 接口

| 方法 | 路径 | 请求体 | 返回 | 说明 |
|---|---|---|---|---|
| GET | `/health` | — | `ok` | 连通性检查 |
| GET | `/chat_text?text=你好` | — | `{text,reply}` | 纯文本对话调试，不走录音 |
| POST | `/asr` | PCM(16k/16bit/单声道) | `{text}` | 一句话识别 |
| POST | `/chat` | PCM | `{text,reply}` | 识别 + DeepSeek 对话（带会话上下文） |
| POST | `/translate` | PCM | `{text,translation}` | 识别 + 翻译（自动判中↔英，无上下文） |
| POST | `/translate_stream` | PCM | 行协议流 | 识别 + DeepSeek **流式**翻译（打字机式） |
| POST | `/translate_ws` | PCM 流(chunked) | 行协议流 | **实时识别**（WebSocket）+ 流式翻译，真·边说边译 |

**行协议**（`/translate_stream` 和 `/translate_ws` 用）：
`T:原文\n` 然后多行 `D:译文增量\n`，每句末 `E\n`。板子逐字节累积遇 `\n` 成行。

## 源码导览

| 文件 | 职责 |
|---|---|
| `main.go` | HTTP 路由 |
| `config.go` | 从环境变量读密钥/配置（fail fast） |
| `aliyun.go` | 阿里云一句话识别：POP 签名换 Token + HTTP 识别 |
| `aliyun_stream.go` | 阿里云**实时识别**：WebSocket 推流 + 自动断句(SentenceEnd) |
| `deepseek.go` | DeepSeek 对话/翻译，含流式(SSE)调用 |
| `translate.go` | 翻译方向判断（有中文→英，否则→中），无上下文一句一译 |
| `session.go` | 会话上下文（多轮记忆，3 分钟超时重置） |
| `resample.go` | PCM 重采样（板子 PDM 采样率→16k；现多已 16k 直出） |
| `wav.go` | 把收到的 PCM 存成 WAV 留档（排查音质，目录已 gitignore） |

## 本地自测（不用板子）

```bash
# 用一个 16k/16bit/单声道的 PCM（WAV 去掉 44 字节头即得）
tail -c +45 some.wav > test.pcm
curl -X POST --data-binary @test.pcm http://localhost:8080/asr
curl -X POST --data-binary @test.pcm "http://localhost:8080/translate_ws"
```
