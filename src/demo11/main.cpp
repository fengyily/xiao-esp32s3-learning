// Demo 11 — 语音转文字（录音 → 上传 → 识别）
// 目标：按一下 D1↔GND → 录 3 秒 → POST 给 Go 中转服务 → 串口打印识别出的文字
// 知识点：I2S 批量录音到 PSRAM、HTTPClient POST 二进制、解析 JSON 响应
//
// 这是对话功能的硬件第一步：板子能"听懂"你说的话（转成文字）。
// 完整对话（板子回答）要等 Go 服务加上 Claude /chat。
//
// 前提：
//   1. 电脑上跑着 Go 中转服务（src/server，监听 :8090）
//   2. 板子和电脑同一 WiFi；secrets.h 里 SERVER_URL 填电脑局域网 IP
//   3. 按钮：D1(GPIO2) ↔ GND
//
// platformio.ini 里 demo11 已开 PSRAM（录音缓冲用）+ -I src/demo07（复用 secrets.h）

#include <Arduino.h>
#include <I2S.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"               // WIFI_SSID / WIFI_PASSWORD / SERVER_URL

#define MIC_CLK   42               // PDM 麦克风（和 Demo 5 一致）
#define MIC_DATA  41
#define SAMPLE_RATE   16000        // 16kHz —— 阿里云一句话识别要求
#define RECORD_SECONDS 3
// PCM 16bit 单声道：16000 采样/秒 × 2 字节 × 秒数
#define PCM_BYTES  (SAMPLE_RATE * 2 * RECORD_SECONDS)   // 3 秒 = 96000 字节

#define BTN_PIN   D1
const unsigned long DEBOUNCE_MS = 50;
int lastReading = HIGH, stableState = HIGH;
unsigned long lastDebounceTime = 0;

uint8_t* recordBuf = nullptr;      // PSRAM 里的录音缓冲

void connectWiFi() {
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 失败，检查 secrets.h / 2.4G。停止。");
    while (true) delay(1000);
  }
  Serial.print("已连接，IP = "); Serial.println(WiFi.localIP());
}

// 录 RECORD_SECONDS 秒 PCM 到 recordBuf，返回实际录到的字节数
size_t recordAudio() {
  // 先读丢一小段，跳过 PDM 麦克风刚启动的杂音
  uint8_t warm[1024];
  for (int i = 0; i < 4; i++) I2S.read(warm, sizeof(warm));

  Serial.println("录音中...（请说话）");
  size_t got = 0;
  while (got < PCM_BYTES) {
    // I2S.read(buf, size) 返回读到的【字节数】
    int n = I2S.read(recordBuf + got, PCM_BYTES - got);
    if (n > 0) got += n;
  }
  Serial.printf("录音完成，%u 字节\n", got);
  return got;
}

// 把 PCM POST 给 Go 服务，解析出识别文字打印
void uploadAndRecognize(size_t pcmLen) {
  HTTPClient http;
  Serial.printf("上传到 %s ...\n", SERVER_URL);
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/octet-stream");

  int code = http.POST(recordBuf, pcmLen);
  if (code == 200) {
    String resp = http.getString();        // 期望 {"text":"..."}
    Serial.println("服务返回: " + resp);

    // 朴素提取 "text":"..." 里的内容（够用；要更稳可上 ArduinoJson）
    int k = resp.indexOf("\"text\"");
    if (k >= 0) {
      int c = resp.indexOf(':', k);
      int q1 = resp.indexOf('"', c + 1);
      int q2 = resp.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 > q1) {
        Serial.println("====> 你说的是: " + resp.substring(q1 + 1, q2));
      }
    }
  } else {
    Serial.printf("上传失败，HTTP %d: %s\n", code, http.getString().c_str());
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Demo 11: 语音转文字 ===");

  pinMode(BTN_PIN, INPUT_PULLUP);

  // PSRAM 申请录音缓冲
  recordBuf = (uint8_t*) ps_malloc(PCM_BYTES);
  if (!recordBuf) {
    Serial.println("PSRAM 分配失败。停止。");
    while (true) delay(1000);
  }
  Serial.printf("录音缓冲 %d 字节（PSRAM）就绪\n", PCM_BYTES);

  // I2S 麦克风（和 Demo 5 一致）
  I2S.setAllPins(-1, MIC_CLK, MIC_DATA, -1, -1);
  if (I2S.begin(PDM_MONO_MODE, SAMPLE_RATE, 16) == 0) {
    Serial.println("I2S 初始化失败。停止。");
    while (true) delay(1000);
  }
  Serial.println("麦克风 OK");

  connectWiFi();
  Serial.println("准备就绪：短接 D1↔GND 开始录音说话");
}

void loop() {
  // 按钮消抖（复用 Demo 3），按下那一刻触发"录音→上传→识别"
  int reading = digitalRead(BTN_PIN);
  if (reading != lastReading) lastDebounceTime = millis();
  if (millis() - lastDebounceTime > DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {
        size_t n = recordAudio();
        uploadAndRecognize(n);
        Serial.println("--- 再按一次可重新录 ---");
      }
    }
  }
  lastReading = reading;
}
