// Demo 13 — 全双工语音对话（双任务：边听边处理，半句不丢）
//
// 问题背景：Demo 11/12 是串行的——录完一句就阻塞着上传+识别+LLM(好几秒)，
//   这期间麦克风没在监听，你接着说的话(如"我想去...钓鱼"的"钓鱼")会丢。
//
// 解决：FreeRTOS 双任务并行（ESP32-S3 双核）：
//   任务A(录音)：永不阻塞，一直听 → VAD 切出一句 → 丢进队列 → 立刻回去听下一句
//   任务B(处理)：从队列取一句 → 上传→识别→DeepSeek → 显示。慢，但不挡录音。
//   队列传"缓冲槽索引"，PCM 放 PSRAM 的 3 个槽轮换。
//
// 硬件：原生 PDM 麦克风 + WiFi，不接线。Go 中转服务的 /chat 带会话上下文。

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include "secrets.h"               // WIFI_SSID / WIFI_PASSWORD / SERVER_URL

#define MIC_CLK   42
#define MIC_DATA  41
#define SAMPLE_RATE  16000
#define I2S_PORT  I2S_NUM_0

// VAD 断句参数
#define MAX_RECORD_SECONDS 10
#define SLOT_BYTES  (SAMPLE_RATE * 2 * MAX_RECORD_SECONDS)  // 每个缓冲槽最大容量
#define SILENCE_END_MS 1200            // 静音 1.2s → 一句结束
#define MIN_RECORD_MS  800             // 至少录 0.8s
// 下面阈值比较的都是【峰峰值】：静音约几百，说话几千。实测后可微调。
#define VAD_LEVEL      1500            // 块峰峰值低于此 = 静音

// 声控触发
const int VOICE_THRESHOLD = 2000;      // 起录阈值(峰峰值)
const unsigned long COOLDOWN_MS = 800; // 一句切完后短冷却，避免余音立刻又触发

// ---------- 多缓冲槽（PSRAM）+ 队列 ----------
#define NUM_SLOTS 3
uint8_t* slots[NUM_SLOTS];             // 3 个录音缓冲，轮流用
size_t   slotLen[NUM_SLOTS];           // 每个槽实际录了多少字节
QueueHandle_t sliceQueue;              // 传"录好的槽索引"给处理任务

// 预缓冲（留住触发那个字）
#define PREROLL_BYTES (SAMPLE_RATE * 2 * 1)
uint8_t* preroll;
volatile size_t prerollLen = 0, prerollPos = 0;

// 串口互斥锁：两个任务都打印，不加锁会交织、把中文(多字节)切成乱码
SemaphoreHandle_t serialMutex;
#define SLOG(...) do { \
    xSemaphoreTake(serialMutex, portMAX_DELAY); \
    Serial.printf(__VA_ARGS__); \
    xSemaphoreGive(serialMutex); \
  } while (0)
// 打印一个完整 String（含中文），整体加锁
void slogLine(const String& s) {
  xSemaphoreTake(serialMutex, portMAX_DELAY);
  Serial.println(s);
  Serial.flush();                  // 等这段真的发完，避免被后续输出/缓冲覆盖
  xSemaphoreGive(serialMutex);
}

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

// 算一块 PCM 的峰峰值(最大-最小)。
// 注意：这个 PDM 麦克风有直流偏置，平均 abs 即使静音也很高(几百~上千)，
// 用平均量判静音会失效；峰峰值不受偏置影响，静音时很小、说话时很大。
int blockLevel(const uint8_t* buf, size_t bytes) {
  const int16_t* s = (const int16_t*)buf;
  size_t n = bytes / 2;
  if (n == 0) return 0;
  int16_t mn = 32767, mx = -32768;
  for (size_t i = 0; i < n; i++) { if (s[i] < mn) mn = s[i]; if (s[i] > mx) mx = s[i]; }
  return mx - mn;
}

// 这段录音里有没有足够的语音（峰峰值法，区分语音 vs 噪声）
bool hasSpeech(const uint8_t* buf, size_t bytes) {
  const int16_t* s = (const int16_t*)buf;
  size_t n = bytes / 2;
  if (n == 0) return false;
  int16_t mn = 32767, mx = -32768;
  for (size_t i = 0; i < n; i++) { if (s[i] < mn) mn = s[i]; if (s[i] > mx) mx = s[i]; }
  return (mx - mn) > 1500;
}

// ===================== 任务A：录音监听（永不阻塞）=====================
void recordTask(void* arg) {
  uint8_t chunk[2048];
  int writeSlot = 0;                     // 当前用哪个槽录
  unsigned long lastTrigger = 0;

  for (;;) {
    // 1. 持续监听 + 填预缓冲，直到音量超阈值才起录
    size_t br = 0;
    i2s_read(I2S_PORT, chunk, sizeof(chunk), &br, portMAX_DELAY);
    // 填预缓冲（环形）
    for (size_t i = 0; i < br; i++) {
      preroll[prerollPos] = chunk[i];
      prerollPos = (prerollPos + 1) % PREROLL_BYTES;
      if (prerollLen < PREROLL_BYTES) prerollLen++;
    }
    int lvl = blockLevel(chunk, br);
    if (lvl <= VOICE_THRESHOLD || millis() - lastTrigger < COOLDOWN_MS) {
      continue;                          // 没说话/冷却中，继续监听
    }

    // 2. 触发！选一个空槽开始录一句
    uint8_t* buf = slots[writeSlot];
    size_t got = 0;

    // 2a. 先拷预缓冲（触发前那一秒，留住开头的字）
    if (prerollLen > 0) {
      if (prerollLen < PREROLL_BYTES) {
        memcpy(buf, preroll, prerollLen);
      } else {
        size_t first = PREROLL_BYTES - prerollPos;
        memcpy(buf, preroll + prerollPos, first);
        memcpy(buf + first, preroll, prerollPos);
      }
      got = prerollLen;
    }

    // 2b. 边录边 VAD 断句
    unsigned long startMs = millis();
    unsigned long lastVoiceMs = startMs;
    while (got < SLOT_BYTES) {
      size_t n = 0;
      size_t want = SLOT_BYTES - got;
      if (want > sizeof(chunk)) want = sizeof(chunk);
      i2s_read(I2S_PORT, buf + got, want, &n, pdMS_TO_TICKS(500));
      if (n > 0) {
        if (blockLevel(buf + got, n) > VAD_LEVEL) lastVoiceMs = millis();
        got += n;
      }
      unsigned long now = millis();
      if (now - startMs > MIN_RECORD_MS && now - lastVoiceMs > SILENCE_END_MS) break;
      if (now - startMs > MAX_RECORD_SECONDS * 1000UL) break;
    }

    lastTrigger = millis();
    prerollLen = 0; prerollPos = 0;       // 清预缓冲，下句重新攒

    // 3. 本地 VAD 过滤：没真说话就丢弃，不入队
    if (!hasSpeech(buf, got)) {
      SLOG("[rec] skip (no speech)\n");      // 纯 ASCII，避免和处理任务中文交织
      continue;
    }

    // 4. 录好一句 → 入队（传槽索引）→ 立刻回去听下一句（关键：不等处理！）
    slotLen[writeSlot] = got;
    SLOG("[rec] slice %.1fs -> slot#%d\n", got / (SAMPLE_RATE * 2.0), writeSlot);
    if (xQueueSend(sliceQueue, &writeSlot, 0) != pdTRUE) {
      SLOG("[rec] queue full, drop\n");
    } else {
      writeSlot = (writeSlot + 1) % NUM_SLOTS;   // 换下一个槽，避免覆盖未处理的
    }
  }
}

// ===================== 任务B：上传处理（慢，不挡录音）=====================
String extractField(const String& json, const char* key) {
  String pat = String("\"") + key + "\"";
  int k = json.indexOf(pat);
  if (k < 0) return "";
  int c = json.indexOf(':', k + pat.length());
  int q1 = json.indexOf('"', c + 1);
  int q2 = json.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 <= q1) return "";
  return json.substring(q1 + 1, q2);
}

void processTask(void* arg) {
  int slot;
  for (;;) {
    // 阻塞等队列里有句子（这个阻塞不影响录音任务）
    if (xQueueReceive(sliceQueue, &slot, portMAX_DELAY) != pdTRUE) continue;

    uint8_t* buf = slots[slot];
    size_t len = slotLen[slot];
    SLOG("[proc] upload slot#%d (%u bytes)...\n", slot, len);

    String url = String(SERVER_URL);     // /chat
    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
      HTTPClient http;
      http.setTimeout(20000);
      http.begin(url);
      http.addHeader("Content-Type", "application/octet-stream");
      int code = http.POST(buf, len);
      if (code == 200) {
        String resp = http.getString();
        String youSaid = extractField(resp, "text");
        String reply   = extractField(resp, "reply");
        // 中文只由本任务打印，且整段一次性输出（录音任务不打中文，避免交织乱码）
        String out = "\n你说: " + youSaid + "\n助手: " + reply + "\n";
        xSemaphoreTake(serialMutex, portMAX_DELAY);
        Serial.print(out);
        Serial.flush();
        xSemaphoreGive(serialMutex);
        ok = true;
      } else {
        SLOG("[proc] HTTP %d (try %d)\n", code, attempt);
        if (attempt < 3) delay(500);
      }
      http.end();
    }
    if (!ok) SLOG("[proc] failed, give up\n");
  }
}

void setup() {
  Serial.setTxBufferSize(4096);      // 加大串口发送缓冲，防长中文串溢出丢字/乱码
  Serial.begin(115200);
  delay(1000);
  serialMutex = xSemaphoreCreateMutex();
  Serial.println("=== Demo 13: 全双工语音对话（边听边处理）===");

  // 分配 PSRAM 缓冲
  for (int i = 0; i < NUM_SLOTS; i++) {
    slots[i] = (uint8_t*) ps_malloc(SLOT_BYTES);
    if (!slots[i]) { Serial.println("PSRAM 分配失败"); while (true) delay(1000); }
  }
  preroll = (uint8_t*) ps_malloc(PREROLL_BYTES);
  if (!preroll) { Serial.println("preroll PSRAM 失败"); while (true) delay(1000); }
  Serial.printf("缓冲就绪：%d 个槽 × %d 字节\n", NUM_SLOTS, SLOT_BYTES);

  sliceQueue = xQueueCreate(NUM_SLOTS, sizeof(int));

  initPDM();
  Serial.println("麦克风 OK");
  connectWiFi();

  // 双任务：录音钉在核心1，处理钉在核心0（WiFi 协议栈也在核心0，没关系）
  xTaskCreatePinnedToCore(recordTask,  "record",  8192,  NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(processTask, "process", 16384, NULL, 1, NULL, 0);

  Serial.println("准备就绪：随时说话，边处理边听，半句不丢。");
}

void loop() {
  // 全部逻辑在两个任务里，主 loop 空转
  vTaskDelay(pdMS_TO_TICKS(1000));
}
