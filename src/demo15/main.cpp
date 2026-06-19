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
#define I2S_PORT  I2S_NUM_0        // 麦克风（PDM RX）

// 喇叭（MAX98357A，I2S TX）用第二个 I2S 口，引脚与 demo16 验证过的一致
#define SPK_PORT  I2S_NUM_1
#define SPK_BCLK  1                // D0/GPIO1 → BCLK
#define SPK_DIN   3                // D2/GPIO3 → DIN
#define SPK_LRC   4                // D3/GPIO4 → LRC（避开损坏的 D1/GPIO2）

// 播报中标志：为 true 时上传任务暂停发麦克风数据，避免喇叭声被录进去形成回授
volatile bool gSpeaking = false;

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

// ===================== 状态灯（单颗板载 LED，靠不同节奏/亮度表达状态）=====================
// 这块板只有 1 颗可控 LED：GPIO21，且【低电平点亮】（duty 越小越亮）。
// 所以用 LEDC(PWM) 调亮度 + 节奏，一颗灯表达多个状态。
// LED 由独立的 ledTask 专门驱动（其它任务只设状态，不碰 LED 硬件，避免争用）。
#define LED_PIN       21
#define LED_LEDC_CH   2          // 用通道2，避开可能的占用
#define LED_FREQ      5000
#define LED_RES_BITS  8          // 8 位：duty 0~255
#define LED_MAX       255        // 低电平点亮：255=灭，0=最亮

enum LedState {
  LED_WIFI,        // 连 WiFi 中：慢闪（亮 100ms / 灭 900ms）
  LED_IDLE,        // 就绪空闲：极暗常亮（醒着但没事干）
  LED_LISTENING,   // 正在听/上传：亮度随音量（说得响→更亮）
  LED_ERROR,       // 出错/断网：快闪（100/100）
};
volatile LedState gLedState = LED_WIFI;
volatile int      gAudioLevel = 0;     // 最近一块音频的峰峰值，喂给 LED_LISTENING

// 写 LED 亮度：0=灭，255=最亮（内部转成低电平点亮的 duty）
static inline void ledSetBrightness(int b) {
  b = constrain(b, 0, 255);
  ledcWrite(LED_LEDC_CH, LED_MAX - b);   // 反相：低电平点亮
}

void initLed() {
  ledcSetup(LED_LEDC_CH, LED_FREQ, LED_RES_BITS);
  ledcAttachPin(LED_PIN, LED_LEDC_CH);
  ledSetBrightness(0);   // 先灭
}

// 专门驱动 LED 的任务：根据 gLedState 画对应的灯效，20ms 刷新一次。
void ledTask(void* arg) {
  unsigned long phase = 0;       // 自增相位，用于闪烁计时
  for (;;) {
    switch (gLedState) {
      case LED_WIFI: {
        // 慢闪：1s 周期，前 100ms 亮
        bool on = (phase % 50) < 5;          // 50*20ms=1s，5*20ms=100ms
        ledSetBrightness(on ? 200 : 0);
        break;
      }
      case LED_IDLE:
        ledSetBrightness(8);                 // 极暗常亮
        break;
      case LED_LISTENING: {
        // 亮度随音量：峰峰值 ~VOICE_THRESHOLD..8000 映射到 30..255
        int lvl = gAudioLevel;
        int b = map(constrain(lvl, 500, 8000), 500, 8000, 20, 255);
        ledSetBrightness(b);
        break;
      }
      case LED_ERROR: {
        bool on = (phase % 10) < 5;          // 200ms 周期快闪
        ledSetBrightness(on ? 255 : 0);
        break;
      }
    }
    phase++;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------- WiFi（带指数退避重连）----------
// 试一轮连接，最多等 timeoutMs。成功返回 true。
bool wifiConnectOnce(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// 确保 WiFi 已连接：没连上就重连，间隔【指数退避】(2→4→8→16→封顶30s)。
// 永不放弃（不再像旧版那样死循环卡住），灯打 LED_WIFI。
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  gLedState = LED_WIFI;
  SLOG("[wifi] 断开/未连，开始重连 %s\n", WIFI_SSID);
  unsigned long backoff = 2000;        // 首次失败后等 2s
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    attempt++;
    SLOG("[wifi] 第 %d 次尝试连接...\n", attempt);
    if (wifiConnectOnce(15000)) {
      SLOG("[wifi] 已连接，IP = %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    SLOG("[wifi] 失败，%lus 后重试\n", backoff / 1000);
    // 退避等待期间灯继续慢闪（ledTask 在跑），这里只是睡
    vTaskDelay(pdMS_TO_TICKS(backoff));
    backoff *= 2;
    if (backoff > 30000) backoff = 30000;   // 封顶 30s，别无限涨
  }
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

// ---------- 喇叭 I2S TX 初始化（MAX98357A）----------
void initSpeaker() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(SPK_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("喇叭 i2s install 失败"); while (true) delay(1000);
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = SPK_BCLK;
  pins.ws_io_num    = SPK_LRC;
  pins.data_out_num = SPK_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(SPK_PORT, &pins);
  i2s_set_clk(SPK_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

// 把一块 PCM 写到喇叭（阻塞直到写完）。
void speakerWrite(const uint8_t* pcm, size_t bytes) {
  size_t sent = 0;
  while (sent < bytes) {
    size_t w = 0;
    i2s_write(SPK_PORT, pcm + sent, bytes - sent, &w, portMAX_DELAY);
    sent += w;
  }
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
    } else if (line.startsWith("P:")) {
      // P:<字节数>\n 之后是该字节数的原始 PCM。PCM 里含 \n，不能按行读，
      // 必须切到按字节读满 N 字节，边读边写喇叭。播报期间置 gSpeaking 暂停上传防回授。
      long total = line.substring(2).toInt();
      SLOG("[播报] %ld 字节音频\n", total);
      gSpeaking = true;
      uint8_t pcmBuf[512];
      long got = 0;
      while (got < total) {
        size_t want = total - got;
        if (want > sizeof(pcmBuf)) want = sizeof(pcmBuf);
        // 阻塞等够这一块（音频是连续流，等待是正常的）
        size_t have = 0;
        unsigned long t0 = millis();
        while (have < want && millis() - t0 < 5000) {
          int n = gClient.read(pcmBuf + have, want - have);
          if (n > 0) have += n;
          else delay(1);
        }
        if (have == 0) break;       // 超时/断流，放弃剩余
        speakerWrite(pcmBuf, have);
        got += have;
      }
      i2s_zero_dma_buffer(SPK_PORT); // 收尾清缓冲，消除空闲底噪
      gSpeaking = false;
      SLOG("[播报] 完成\n");
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
    // 0. 每轮先确保 WiFi 在线（运行中掉线会自动重连，带退避）
    ensureWiFi();
    gLedState = LED_IDLE;          // 就绪空闲：极暗常亮

    // 1. 监听，等有人开口
    size_t br = 0;
    i2s_read(I2S_PORT, chunk, sizeof(chunk), &br, portMAX_DELAY);
    int lvl0 = blockLevel(chunk, br);
    gAudioLevel = lvl0;            // 即使空闲也更新（暂未用于 IDLE，但保持新鲜）
    if (lvl0 <= VOICE_THRESHOLD) continue;

    unsigned long sessionStart = millis();   // 开口时刻，作为所有耗时的基准
    gLedState = LED_LISTENING;     // 进入"正在听"：亮度随音量

    // 2. 连服务、发 chunked 流式上传请求头
    SLOG("[up] 检测到说话，建立连接...\n");
    if (!gClient.connect(gHost.c_str(), gPort)) {
      SLOG("[up] 连接服务失败\n");
      gLedState = LED_ERROR;       // 连不上服务：报错快闪
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
      if (n > 0 && !gSpeaking) {            // 播报期间只读不发，避免把喇叭声录进去回授
        sendChunk(chunk, n);
        int lvl = blockLevel(chunk, n);
        gAudioLevel = lvl;                  // 喂给状态灯：亮度随音量
        if (lvl > VOICE_THRESHOLD) lastVoiceMs = millis();
      }
      if (gSpeaking) lastVoiceMs = millis(); // 播报不算静音，别在播报后立刻收尾
      drainResponse(parser);     // 同一任务里顺便把已到的译文读出来（播 P: 时会阻塞播放）
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
    gLedState = LED_IDLE;        // 回到就绪空闲
    SLOG("[up] 连接已关，本次会话总时长 %lums，回到监听\n", millis() - sessionStart);
  }
}

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(1000);
  serialMutex = xSemaphoreCreateMutex();
  Serial.println("=== Demo 15: 流式实时翻译（阿里云实时识别）===");

  // 状态灯先起来：连 WiFi 期间就能看到慢闪
  initLed();
  gLedState = LED_WIFI;
  xTaskCreatePinnedToCore(ledTask, "led", 2048, NULL, 1, NULL, 0);

  initPDM();
  Serial.println("麦克风 OK");
  initSpeaker();
  Serial.println("喇叭 OK");
  ensureWiFi();                  // 带指数退避重连，连不上不再死循环卡死
  parseServerURL();

  // 单任务全双工：录音/上传/读响应都在这一个任务里，独占 socket（避免 lwIP 跨核崩溃）。
  // 钉在核心1，把核心0留给 WiFi 协议栈。
  xTaskCreatePinnedToCore(streamTask, "stream", 12288, NULL, 2, NULL, 1);

  Serial.println("准备就绪：开口即开始流式识别，阿里云自动断句，边说边译。");
  Serial.println("状态灯：慢闪=连WiFi  暗亮=就绪  随音量变亮=正在听  快闪=出错");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
