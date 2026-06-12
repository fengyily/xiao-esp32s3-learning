// 纯录音测试 —— 用 ESP-IDF 原生 I2S(legacy driver/i2s.h) 正确配置 PDM RX
// 背景：Arduino I2S 库的 PDM 实现把高频砍到 2kHz（mclk/下采样配错），录音又闷又糊。
//       这里用原生 API，显式设对 PDM 下采样，拿到清晰的 16kHz 录音。
//
// 流程：一开机连 WiFi → 录 4 秒 → POST 给 Go /asr → Go 存 WAV。反复录，专心调音质。
// 不声控、不对话，最纯粹地验证录音质量。

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include "secrets.h"               // WIFI_SSID / WIFI_PASSWORD / SERVER_URL

#define MIC_CLK   42               // PDM 时钟
#define MIC_DATA  41               // PDM 数据
#define SAMPLE_RATE 16000          // 目标 16kHz
#define RECORD_SECONDS 4
#define PCM_BYTES  (SAMPLE_RATE * 2 * RECORD_SECONDS)
#define I2S_PORT  I2S_NUM_0

uint8_t* recordBuf = nullptr;

// 用原生 API 初始化 PDM RX，正确配置下采样
void initPDM() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // 单声道
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
  pins.ws_io_num    = MIC_CLK;       // PDM 模式下 ws = PDM 时钟
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_DATA;      // PDM 数据
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("i2s_set_pin 失败"); while (true) delay(1000);
  }

  // 关键：显式设 PDM RX 下采样率（老库漏了这步，导致高频丢失）
  i2s_set_pdm_rx_down_sample(I2S_PORT, I2S_PDM_DSR_8S);   // 8 倍下采样
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  Serial.println("PDM RX 初始化完成（原生 API）");
}

void connectWiFi() {
  Serial.printf("连 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi 失败"); while (true) delay(1000); }
  Serial.print("IP = "); Serial.println(WiFi.localIP());
}

size_t recordAudio() {
  // 读丢启动杂音
  uint8_t warm[2048]; size_t br;
  for (int i = 0; i < 8; i++) i2s_read(I2S_PORT, warm, sizeof(warm), &br, portMAX_DELAY);

  Serial.println("录音中（4 秒，请说话）...");
  size_t got = 0;
  while (got < PCM_BYTES) {
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, recordBuf + got, PCM_BYTES - got, &bytesRead, portMAX_DELAY);
    got += bytesRead;
  }
  Serial.printf("录音完成 %u 字节（%.1f 秒 @ %dHz）\n", got, got / (SAMPLE_RATE * 2.0), SAMPLE_RATE);
  return got;
}

void uploadAudio(size_t len) {
  // 原生 PDM 已正确出 16kHz，不用重采样，直接传（src_rate=16000，Go 不会动它）
  String url = String(SERVER_URL);   // SERVER_URL 是 /chat，这里改用 /asr 只存不对话
  url.replace("/chat", "/asr");
  Serial.printf("上传 %s ...\n", url.c_str());
  HTTPClient http;
  http.setTimeout(20000);
  http.begin(url);
  http.addHeader("Content-Type", "application/octet-stream");
  int code = http.POST(recordBuf, len);
  Serial.printf("HTTP %d: %s\n", code, code == 200 ? http.getString().c_str() : "");
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== 纯录音测试（原生 PDM）===");
  recordBuf = (uint8_t*) ps_malloc(PCM_BYTES);
  if (!recordBuf) { Serial.println("PSRAM 失败"); while (true) delay(1000); }
  initPDM();
  connectWiFi();
  Serial.println("准备好。每隔几秒自动录一次，对着板子说话。");
}

void loop() {
  size_t n = recordAudio();
  uploadAudio(n);
  Serial.println("--- 3 秒后再录 ---\n");
  delay(3000);
}
