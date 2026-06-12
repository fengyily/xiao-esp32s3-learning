// Demo 12 — 实时中英互译（声控录音 → 识别 → 翻译 → 打印译文）
// 说中文出英文、说英文出中文（Go 端自动判方向）。后续接喇叭可语音播报。
// 板子端与 Demo 11 几乎相同，只是上传到 /translate、解析 translation 字段。
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
#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"            // 原生 I2S：正确配置 PDM RX（老 I2S 库会丢高频）
#include "secrets.h"               // WIFI_SSID / WIFI_PASSWORD / SERVER_URL

#define MIC_CLK   42               // PDM 时钟
#define MIC_DATA  41               // PDM 数据
#define SAMPLE_RATE  16000         // 原生 PDM 正确出 16kHz（不再需要 Go 重采样）
#define I2S_PORT  I2S_NUM_0
#define RECORD_SECONDS 5           // 给说话留足时间
#define PCM_BYTES  (SAMPLE_RATE * 2 * RECORD_SECONDS)

// 声控触发：监听音量，超过阈值就开始录音（不用按钮，D1/GPIO2 已损坏弃用）
const int VOICE_THRESHOLD = 1500;           // 触发阈值（实测说话约1360；偶尔空录无害，会回"请再说一次"）
const unsigned long COOLDOWN_MS = 1000;     // 翻译场景：1 秒，连续翻译跟手（无喇叭不怕自触发）
unsigned long lastTrigger = 0;

uint8_t* recordBuf = nullptr;      // PSRAM 里的录音缓冲

// 原生 PDM RX 初始化（正确配置下采样，录音才清晰；老 I2S 库会丢高频）
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
    Serial.println("i2s_driver_install 失败"); while (true) delay(1000);
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_PIN_NO_CHANGE;
  pins.ws_io_num    = MIC_CLK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_DATA;
  i2s_set_pin(I2S_PORT, &pins);
  i2s_set_pdm_rx_down_sample(I2S_PORT, I2S_PDM_DSR_8S);   // 关键：正确下采样
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

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
// 预缓冲：持续保存最近 PREROLL 字节的音频，触发时拼到录音开头，
// 这样"触发那个字"（如"你好"的"你"）不会被丢。
#define PREROLL_BYTES (SAMPLE_RATE * 2 * 1)   // 约 1 秒预缓冲
uint8_t preroll[PREROLL_BYTES];
size_t  prerollLen = 0;     // 当前已填多少（未满 1 秒时）
size_t  prerollPos = 0;     // 环形写入位置

// 边读音频边算音量；同时把这批音频塞进预缓冲环形区。返回平均音量。
int readLevelKeep() {
  int16_t buf[256];
  size_t br = 0;
  i2s_read(I2S_PORT, buf, sizeof(buf), &br, portMAX_DELAY);
  int got = (int)br;
  int samples = got / 2;

  long sum = 0;
  for (int i = 0; i < samples; i++) sum += abs(buf[i]);

  // 存进预缓冲（环形覆盖最旧的）
  for (int i = 0; i < got; i++) {
    preroll[prerollPos] = ((uint8_t*)buf)[i];
    prerollPos = (prerollPos + 1) % PREROLL_BYTES;
    if (prerollLen < PREROLL_BYTES) prerollLen++;
  }
  return samples ? (int)(sum / samples) : 0;
}

size_t recordAudio() {
  Serial.println("录音中...（请说话）");
  size_t got = 0;

  // 1. 先把预缓冲里"触发前那一段"按时间顺序拷到录音开头
  if (prerollLen > 0) {
    if (prerollLen < PREROLL_BYTES) {
      // 还没写满一圈：数据从 0 到 prerollPos
      memcpy(recordBuf, preroll, prerollLen);
    } else {
      // 写满了：最旧的从 prerollPos 开始，绕一圈
      size_t firstPart = PREROLL_BYTES - prerollPos;
      memcpy(recordBuf, preroll + prerollPos, firstPart);
      memcpy(recordBuf + firstPart, preroll, prerollPos);
    }
    got = prerollLen;
  }

  unsigned long startMs = millis();
  const unsigned long TIMEOUT_MS = (RECORD_SECONDS + 2) * 1000UL;
  while (got < PCM_BYTES) {
    size_t br = 0;
    i2s_read(I2S_PORT, recordBuf + got, PCM_BYTES - got, &br, pdMS_TO_TICKS(500));
    got += br;
    if (millis() - startMs > TIMEOUT_MS) {
      Serial.println("（录音超时，用已录到的部分）");
      break;
    }
  }
  // 报告这段录音的峰值占满量程的比例，提示是否削波（离麦远点/小声点能避免）
  {
    int16_t* s = (int16_t*) recordBuf;
    size_t cnt = got / 2;
    int16_t peak = 0;
    for (size_t i = 0; i < cnt; i++) {
      int16_t a = s[i] < 0 ? -s[i] : s[i];
      if (a > peak) peak = a;
    }
    if (peak >= 32000) {
      Serial.println("⚠️ 录音削波饱和！请离麦克风远一点、说话别太大声，否则识别会乱。");
    }
  }

  Serial.printf("录音完成，%u 字节（%.1f 秒）\n", got, got / (SAMPLE_RATE * 2.0));
  return got;
}

// 从 JSON 里朴素提取 "key":"value" 的 value（够用；要更稳可上 ArduinoJson）
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

// 粗略判断这段录音里有没有"足够的语音"——统计响度超过说话线的样本占比。
// 用来在本地拦掉"误触发但没真说话"的录音，省去无谓的上传+识别。
bool hasSpeech(size_t pcmLen) {
  int16_t* s = (int16_t*) recordBuf;
  size_t n = pcmLen / 2;
  if (n == 0) return false;

  // 诊断：看这段录音的统计特征，区分 语音(有起伏) / 噪声(恒大) / 静音(恒小)
  long sum = 0; int16_t mn = 32767, mx = -32768;
  for (size_t i = 0; i < n; i++) {
    sum += abs(s[i]);
    if (s[i] < mn) mn = s[i];
    if (s[i] > mx) mx = s[i];
  }
  int avgAbs = (int)(sum / n);
  int range = mx - mn;               // 峰峰值（动态范围）
  Serial.printf("[诊断] 平均=%d 最小=%d 最大=%d 峰峰值=%d\n", avgAbs, mn, mx, range);

  // 关键判据：真语音上下振动大(峰峰值大、有负值)；噪声/直流偏置摆动小。
  // 实测：说话峰峰值 3000+，噪声仅约 666。阈值定 1500 干净区分。
  return range > 1500;
}

// 把 PCM POST 给 Go 服务的 /translate，解析原文 + 译文
void uploadAndTranslate(size_t pcmLen) {
  // 原生 PDM 已正确出 16kHz，直接上传。SERVER_URL 是 /chat，这里换成 /translate
  String url = String(SERVER_URL);
  url.replace("/chat", "/translate");
  Serial.printf("上传到 %s ...\n", url.c_str());

  // 最多试 3 次：HTTP -1 多是 WiFi/连接瞬时抖动，重试通常能成
  for (int attempt = 1; attempt <= 3; attempt++) {
    HTTPClient http;
    http.setTimeout(20000);                 // 给上传+云端处理留够时间
    http.begin(url);
    http.addHeader("Content-Type", "application/octet-stream");
    int code = http.POST(recordBuf, pcmLen);

    if (code == 200) {
      String resp = http.getString();       // 期望 {"text":"...","translation":"..."}
      Serial.println("服务返回: " + resp);
      String youSaid = extractField(resp, "text");
      String trans   = extractField(resp, "translation");
      if (youSaid.length()) Serial.println("====> 原文: " + youSaid);
      if (trans.length())   Serial.println("====> 译文: " + trans);
      http.end();
      return;                               // 成功，结束
    }

    Serial.printf("请求失败(第%d次)，HTTP %d\n", attempt, code);
    http.end();
    if (attempt < 3) delay(500);            // 稍等再重试
  }
  Serial.println("三次都失败，放弃这条。");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Demo 12: 实时中英互译（说话→翻译→打印）===");

  // PSRAM 申请录音缓冲
  recordBuf = (uint8_t*) ps_malloc(PCM_BYTES);
  if (!recordBuf) {
    Serial.println("PSRAM 分配失败。停止。");
    while (true) delay(1000);
  }
  Serial.printf("录音缓冲 %d 字节（PSRAM）就绪\n", PCM_BYTES);

  initPDM();                               // 原生 PDM RX 初始化（录音清晰的关键）
  Serial.println("麦克风 OK");

  connectWiFi();
  Serial.printf("准备就绪：直接对板子说话即可（音量超过 %d 触发录音）\n", VOICE_THRESHOLD);
}

void loop() {
  // 持续监听音量（同时把音频留进预缓冲，留住"触发那个字"）
  int level = readLevelKeep();

  if (level > VOICE_THRESHOLD && millis() - lastTrigger > COOLDOWN_MS) {
    Serial.printf("听到声音(level=%d)，开始录音…\n", level);
    size_t n = recordAudio();
    // 本地先判断有没有真说话，没有就不上传，省一次请求
    if (hasSpeech(n)) {
      uploadAndTranslate(n);
    } else {
      Serial.println("没检测到有效语音，跳过上传。");
    }
    // 清空预缓冲 + 进入冷却，避免回答余音/旧音频混入下一轮
    prerollLen = 0; prerollPos = 0;
    lastTrigger = millis();
    Serial.printf("--- 冷却 %lu 秒后可再次说话 ---\n", COOLDOWN_MS / 1000);
  }
}
