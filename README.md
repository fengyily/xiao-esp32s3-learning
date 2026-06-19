# XIAO ESP32-S3 Sense 学习项目

从 0 基础起步，用一个个能跑起来、看得见效果的 Demo 学嵌入式开发，
最终做出一个**口袋里的 AI 语音助手 / 实时翻译机**。

**当前进度**：基础/联网/语音全部跑通（Demo 1-15）。实时翻译已达"边说边译"，
说完一句约 0.7-1.3s 出译文。差最后"语音播报"一环（等 MAX98357A 喇叭到货）。

> 配套文档：
> - [LEARNING_PLAN.md](LEARNING_PLAN.md) — 完整学习路线、进度日志、踩过的坑
> - [DIALOGUE_PLAN.md](DIALOGUE_PLAN.md) — 语音助手的架构设计与分阶段实现
> - [src/server/README.md](src/server/README.md) — Go 中转服务说明

## 硬件

**Seeed XIAO ESP32-S3 Sense** —— 指甲盖大小，但很全：

- ESP32-S3 双核 + WiFi/BLE，8MB PSRAM + 8MB Flash
- OV2640 摄像头、PDM 数字麦克风、SD 卡槽
- 板载橙色 LED：接 **GPIO21**，**低电平点亮**（写 `LOW` 才亮，常见坑）

## 环境准备

需要 [PlatformIO](https://platformio.org/)（VS Code 插件或 CLI）。

```bash
# PATH 里要有 pio（按需加进 ~/.zshrc）
export PATH="$PATH:$HOME/.platformio/penv/bin"

pio run -e demo01 -t upload      # 编译 + 烧录某个 demo
pio device monitor               # 看串口输出（115200）
```

涉及 WiFi 的 demo 需要配 `secrets.h`：

```bash
cp src/demo07/secrets.h.example src/demo07/secrets.h   # 填入你的 WiFi 和服务地址
```

> `secrets.h`、`.env`、录音、照片都在 [.gitignore](.gitignore) 里，不会上传。

## Demo 一览

每个 demo 是一个独立可烧录的程序，对应 `platformio.ini` 里的一个 `[env:demoNN]`，
用 `pio run -e demoNN -t upload` 烧录。

### 基础（输入输出、外设）

| Demo | 内容 |
|---|---|
| 01 | Blink 闪灯 —— GPIO、`setup`/`loop` |
| 02 | 串口对话 —— `Serial` 调试 |
| 03 | 按钮输入 —— 上拉、消抖、下降沿（D1↔GND 模拟按钮）|
| 04 | PWM 呼吸灯 —— LEDC 调亮度 |
| 05 | 读麦克风音量 —— I2S/PDM 采集 + 串口音量条 |
| 06 | 摄像头拍照存 SD 卡 —— OV2640 + SPI SD（CS=21）|

### 联网

| Demo | 内容 |
|---|---|
| 07 | 连 WiFi + NTP 网络对时 |
| 08 | 网页看摄像头实时画面 —— MJPEG 视频流 |
| 100 | SD 卡读写测试（SPI 方式，用来排查 Demo 6 的挂载问题）|

### 语音助手 / 实时翻译（核心）

板子算力跑不动 ASR/LLM，且阿里云签名在 ESP32 难写，所以加一层 **Go 中转服务**：
板子只录音/收文字，签名鉴权、识别、对话翻译全在 Go 端（密钥也只留服务端）。

```text
                         实时识别(WebSocket)        流式翻译
 [板子 ESP32-S3]            ┌──────────┐          ┌──────────┐
  录音 ──PCM流(chunked)──▶ │ Go 中转  │ ──音频──▶│ 阿里云ASR │
  状态灯/串口显示  ◀─T/D/E─ │  服务    │ ◀─句子── └──────────┘
                          │          │ ──文字──▶┌──────────┐
                          └──────────┘ ◀─译文增量│ DeepSeek │
                                                 └──────────┘
```

| Demo | 内容 |
|---|---|
| 11 | 语音转文字 + DeepSeek 对话 —— 声控触发、原生 PDM、VAD、免提语音助手 |
| 12 | 实时中英互译 —— 说话→识别→翻译→打印译文 |
| 13 | **全双工对话** —— FreeRTOS 双任务边听边处理（半句不丢）+ 会话上下文记忆 |
| 14 | **全双工流式翻译** —— 双任务 + DeepSeek 流式输出（译文打字机式）|
| 15 | **流式实时翻译** —— 阿里云实时识别(WebSocket)自动断句，真·边说边译；带 WiFi 退避重连 + 状态灯 |

> `diag` / `micdiag` / `micrec` 是诊断小程序（引脚损坏排查、采样率实测、纯录音测试）。

## 跑通语音翻译（Demo 15）

1. **起 Go 服务**（需阿里云 + DeepSeek 密钥，见 [src/server/README.md](src/server/README.md)）：
   ```bash
   cd src/server && cp .env.example .env   # 填密钥
   set -a && source .env && set +a && go run .
   ```
2. **配板子**：`cp src/demo07/secrets.h.example src/demo07/secrets.h`，
   填 WiFi 和 `SERVER_URL`（你跑 Go 服务那台机器的局域网 IP，如 `http://192.168.x.x:8090/chat`）。
3. **烧录**：`pio run -e demo15 -t upload`，开串口对着板子说话。

**状态灯**（板载单灯）：慢闪=连 WiFi｜暗亮=就绪｜随音量变亮=正在听｜快闪=出错。

## 我踩过的坑

详见 [LEARNING_PLAN.md 的"我学到的坑"](LEARNING_PLAN.md#我学到的坑随手记)。几个高频的：

- 板载 LED **低电平点亮**，PWM 调亮度时 duty 要反着写。
- ESP32-S3 **只支持 2.4G WiFi**。
- PDM 录音又闷又糊 → 用原生 `driver/i2s.h` + `i2s_set_pdm_rx_down_sample`，别用旧 Arduino I2S 库。
- VAD 判静音用**峰峰值**(max-min)，别用平均量（PDM 有直流偏置，平均量恒高）。
- **WiFiClient/lwIP 不是线程安全**：一个 socket 不能两个任务同时读写，会崩 `pbuf_free`。
- 实时翻译断句靠阿里云 `max_sentence_silence`（400ms 实测合适），别用固定时长切片切碎单词。

## 目录结构

```text
src/
  demo01../demo15/   各 demo 的 main.cpp
  demo100/           SD 卡测试
  diag/ micdiag/ micrec/   诊断程序
  server/            Go 中转服务（ASR + DeepSeek）
tools/recv_photo.py  串口 dump 照片的解码脚本
platformio.ini       多 demo 构建配置
```
