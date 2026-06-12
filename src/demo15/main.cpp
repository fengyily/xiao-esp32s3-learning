// Demo 15 — 流式实时翻译（阿里云实时识别 ASR）
//
// 和 Demo 14 的根本区别：不再由板子做 VAD 切片再整段识别。
//   板子：持续录音，用裸 WiFiClient 把 PCM【不间断流式上传】到 Go 服务。
//   Go  ：转推阿里云【实时语音识别】WS，由阿里云在【语义停顿】处自动断句，
//          一句说完就翻译，流式回吐。
//   板子：同一个 socket 上【边传边读】响应（全双工），打印 原文/译文。
//
// 这样切句由专业 ASR 决定（不会把单词切碎），延时低、连续说不丢字。
//
// 硬件：原生 PDM 麦克风 + WiFi，不接线。
// 服务端：Go 的 /translate_ws（SERVER_URL 把 /chat 换成 /translate_ws）。

#include <Arduino.h>
#include <WiFi.h>
#include "driver/i2s.h"
#include "secrets.h"               // WIFI_SSID / WIFI_PASSWORD / SERVER_URL

#define MIC_CLK   42
#define MIC_DATA  41
#define SAMPLE_RATE  16000
#define I2S_PORT  I2S_NUM_0

// 一次会话最多录多久（防跑飞）；到时间自动收尾，可重开。
#define MAX_SESSION_SECONDS 120

// 起录/收尾的本地粗判（断句交给阿里云，这里只决定"何时开始/结束整段会话"）
const int  VOICE_THRESHOLD = 2000;     // 峰峰值高于此 = 有人开始说话
#define SESSION_SILENCE_MS 8000        // 连续 8s 没人说话 → 结束本次会话连接

// 串口互斥：上传任务(ASCII)和读响应任务(中文)都打印，加锁防 UTF-8 被切碎
SemaphoreHandle_t serialMutex;
#define SLOG(...) do { \
    xSemaphoreTake(serialMutex, portMAX_DELAY); \
    Serial.printf(__VA_ARGS__); \
    xSemaphoreGive(serialMutex); \
  } while (0)

// socket：单任务独占（边写音频边读响应），不跨任务共享，避免 lwIP 跨核崩溃。
WiFiClient gClient;

// 解析 SERVER_URL 得到的 host/port（path 固定用 /translate_ws）
String gHost;
int    gPort = 80;

// ---------- WiFi ----------
void connectWiFi() {
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi 失败。停止。"); while (true) delay(1000); }
  Serial.print("已连接，IP = "); Serial.println(WiFi.localIP());
}

// 从 "http://192.168.10.11:8090/chat" 解析出 host 和 port
void parseServerURL() {
  String u = String(SERVER_URL);
  u.replace("http://", "");
  int slash = u.indexOf('/');
  if (slash >= 0) u = u.substring(0, slash);   // 去掉路径，只留 host:port
  int colon = u.indexOf(':');
  if (colon >= 0) {
    gHost = u.substring(0, colon);
    gPort = u.substring(colon + 1).toInt();
  } else {
    gHost = u; gPort = 80;
  }
  Serial.printf("服务: %s:%d  路径: /translate_ws\n", gHost.c_str(), gPort);
}

// ---------- 原生 PDM 初始化 ----------
void initPDM() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 1024;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("i2s install 失败"); while (true) delay(1000);
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num  = MIC_CLK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_DATA;
  i2s_set_pin(I2S_PORT, &pins);
  i2s_set_pdm_rx_down_sample(I2S_PORT, I2S_PDM_DSR_8S);
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

// 一块 PCM 的峰峰值（最大-最小）：静音几百，说话几千。PDM 有直流偏置，不能用平均量。
int blockLevel(const uint8_t* buf, size_t bytes) {
  const int16_t* s = (const int16_t*)buf;
  size_t n = bytes / 2;
  if (n == 0) return 0;
  int16_t mn = 32767, mx = -32768;
  for (size_t i = 0; i < n; i++) { if (s[i] < mn) mn = s[i]; if (s[i] > mx) mx = s[i]; }
  return mx - mn;
}

// ---------- 解析响应的状态（在单任务里边传边读用）----------
// 重要：ESP32 的 WiFiClient/lwIP【不是线程安全】的——同一个 socket 不能在两个
// 任务/两个核心里同时读写，否则 pbuf 引用计数错乱，触发
//   assert failed: pbuf_free ... p->ref > 0
// 这是上一版崩溃的根因。所以这一版【单任务全双工】：同一个任务里既写音频又读响应。
struct RespParser {
  String lineBuf;
  bool   inHeader;
  // 计时用：
  unsigned long sessionStart;   // 本次会话起点（开口/连接的时刻）
  unsigned long tStart;         // 当前句收到 T: 原文的时刻
  bool          firstDelta;     // 当前句是否还没收到第一段译文
  unsigned long firstDeltaMs;   // 第一段译文相对 T: 的耗时
  void reset(unsigned long now) {
    lineBuf = ""; inHeader = true;
    sessionStart = now; tStart = 0; firstDelta = true; firstDeltaMs = 0;
  }
};

// 把 socket 里【当前已到达】的字节读出来解析（非阻塞，读空即返回）。
// 不阻塞，才能立刻回去继续录音上传，实现"边说边出译文"。
// 同时打印每句的耗时：识别耗时(开口→出原文) / 首字译文耗时 / 整句译文耗时。
void drainResponse(RespParser& p) {
  while (gClient.available()) {
    char c = (char) gClient.read();

    if (p.inHeader) {
      // 跳过 HTTP 响应头，遇空行(\r\n\r\n)进入正文
      if (c == '\n') {
        if (p.lineBuf == "\r" || p.lineBuf == "") p.inHeader = false;
        p.lineBuf = "";
      } else {
        p.lineBuf += c;
      }
      continue;
    }

    if (c != '\n') { p.lineBuf += c; continue; }
    String line = p.lineBuf; p.lineBuf = "";
    if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);

    unsigned long now = millis();
    // 单任务，串口不会被并发写，但仍整体加锁保持习惯一致
    if (line.startsWith("T:")) {
      p.tStart = now;
      p.firstDelta = true;
      // 从开口到这句原文出来 = 识别延时（含说这句的时长 + 网络 + ASR）
      unsigned long asrMs = now - p.sessionStart;
      xSemaphoreTake(serialMutex, portMAX_DELAY);
      Serial.printf("\n[识别 +%lums] 原文: %s\n", asrMs, line.substring(2).c_str());
      Serial.print("译文: "); Serial.flush();
      xSemaphoreGive(serialMutex);
    } else if (line.startsWith("D:")) {
      if (p.firstDelta) {                       // 记首字译文延时（原文→第一段译文）
        p.firstDeltaMs = now - p.tStart;
        p.firstDelta = false;
      }
      xSemaphoreTake(serialMutex, portMAX_DELAY);
      Serial.print(line.substring(2)); Serial.flush();
      xSemaphoreGive(serialMutex);
    } else if (line.startsWith("E")) {
      unsigned long transMs = now - p.tStart;   // 原文→整句译文完
      xSemaphoreTake(serialMutex, portMAX_DELAY);
      Serial.printf("\n[本句 首字译文 +%lums | 整句译文 +%lums]\n", p.firstDeltaMs, transMs);
      Serial.flush();
      xSemaphoreGive(serialMutex);
    }
  }
}

// ===================== 单任务：录音 + 上传 + 读响应（全双工，独占 socket）=====================
// 一个任务里完成：等开口 → 连服务发头 → 循环{录一块→发一块→读已到的响应} →
// 长静音收尾 → 把尾句译文读完 → 关连接。socket 只被这一个任务摸，lwIP 安全。
void streamTask(void* arg) {
  uint8_t chunk[3200];   // 100ms 一块
  RespParser parser;
  for (;;) {
    // 1. 监听，等有人开口
    size_t br = 0;
    i2s_read(I2S_PORT, chunk, sizeof(chunk), &br, portMAX_DELAY);
    if (blockLevel(chunk, br) <= VOICE_THRESHOLD) continue;

    unsigned long sessionStart = millis();   // 开口时刻，作为所有耗时的基准

    // 2. 连服务、发 chunked 流式上传请求头
    SLOG("[up] 检测到说话，建立连接...\n");
    if (!gClient.connect(gHost.c_str(), gPort)) {
      SLOG("[up] 连接服务失败\n");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    SLOG("[up] 连接耗时 %lums\n", millis() - sessionStart);
    gClient.print(String("POST /translate_ws HTTP/1.1\r\n"));
    gClient.print("Host: " + gHost + "\r\n");
    gClient.print("Content-Type: application/octet-stream\r\n");
    gClient.print("Connection: close\r\n");
    gClient.print("Transfer-Encoding: chunked\r\n");
    gClient.print("\r\n");
    parser.reset(sessionStart);

    // 辅助：发一个 chunked 数据块
    auto sendChunk = [&](const uint8_t* data, size_t len) {
      char hdr[16];
      int hl = snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned)len);
      gClient.write((const uint8_t*)hdr, hl);
      gClient.write(data, len);
      gClient.print("\r\n");
    };

    // 先把触发那一块也发上去（留住开头的字）
    sendChunk(chunk, br);

    // 3. 边录边传边读：录一块→发一块→把已到的响应读出来打印
    unsigned long startMs = millis();
    unsigned long lastVoiceMs = startMs;
    while (gClient.connected()) {
      size_t n = 0;
      i2s_read(I2S_PORT, chunk, sizeof(chunk), &n, pdMS_TO_TICKS(200));
      if (n > 0) {
        sendChunk(chunk, n);
        if (blockLevel(chunk, n) > VOICE_THRESHOLD) lastVoiceMs = millis();
      }
      drainResponse(parser);     // 同一任务里顺便把已到的译文读出来（不阻塞）
      unsigned long now = millis();
      if (now - lastVoiceMs > SESSION_SILENCE_MS) { SLOG("[up] 长静音，结束本次会话\n"); break; }
      if (now - startMs > MAX_SESSION_SECONDS * 1000UL) { SLOG("[up] 达最大时长，结束\n"); break; }
    }

    // 4. 收尾：发 chunked 结束块，让服务端识别尾句；继续读完剩余译文
    gClient.print("0\r\n\r\n");
    unsigned long waitStart = millis();
    while (gClient.connected() && millis() - waitStart < 10000) {
      drainResponse(parser);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    drainResponse(parser);       // 关前最后再扫一遍
    gClient.stop();
    SLOG("[up] 连接已关，本次会话总时长 %lums，回到监听\n", millis() - sessionStart);
  }
}

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(1000);
  serialMutex = xSemaphoreCreateMutex();
  Serial.println("=== Demo 15: 流式实时翻译（阿里云实时识别）===");

  initPDM();
  Serial.println("麦克风 OK");
  connectWiFi();
  parseServerURL();

  // 单任务全双工：录音/上传/读响应都在这一个任务里，独占 socket（避免 lwIP 跨核崩溃）。
  // 钉在核心1，把核心0留给 WiFi 协议栈。
  xTaskCreatePinnedToCore(streamTask, "stream", 12288, NULL, 2, NULL, 1);

  Serial.println("准备就绪：开口即开始流式识别，阿里云自动断句，边说边译。");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
