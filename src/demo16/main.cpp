// Demo 16：喇叭播放测试（MAX98357A I2S 功放）
// 目的：最简单地验证 MAX98357A 接线正确、I2S 播放通路打通——开机自动循环播放提示音。
//       不连 WiFi、不依赖服务器，纯本地生成正弦波喂给 I2S，专心调通硬件。
//
// 接线（XIAO ESP32-S3 → MAX98357A）：
//   3V3  → Vin
//   GND  → GND
//   D0/GPIO1 → BCLK   (位时钟)
//   D2/GPIO3 → LRC    (左右声道时钟 / WS)
//   D3/GPIO4 → DIN    (数据)
//   GAIN、SD 留空即可（默认增益 9dB，SD 内部上拉为播放）
//   （避开 D1/GPIO2——本项目里它被标记为损坏；也避开麦克风占用的 GPIO41/42）
//
// 喇叭直接焊在 MAX98357A 的 + / - 输出端子上（8Ω 0.5~3W 均可）。

#include <Arduino.h>
#include <math.h>
#include "driver/i2s.h"
#include "voice.h"                 // 预合成的中文语音 PCM（macOS say 生成）

#define SPK_BCLK  1                 // D0/GPIO1 → BCLK
#define SPK_DIN   3                 // D2/GPIO3 → DIN
#define SPK_LRC   4                 // D3/GPIO4 → LRC (WS)  （避开损坏的 D1/GPIO2）
#define SAMPLE_RATE 16000
#define I2S_PORT  I2S_NUM_1         // 麦克风用 NUM_0，喇叭用 NUM_1，互不干扰

// 初始化 I2S TX（标准 Philips I2S，单声道喂给 MAX98357A）
void initSpeaker() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // MAX98357A 默认取左声道
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;                    // 缓冲空了自动清零，避免爆音

  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("i2s_driver_install 失败"); while (true) delay(1000);
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = SPK_BCLK;
  pins.ws_io_num    = SPK_LRC;
  pins.data_out_num = SPK_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("i2s_set_pin 失败"); while (true) delay(1000);
  }

  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  Serial.printf("喇叭 I2S TX 初始化完成（%dHz）\n", SAMPLE_RATE);
}

// 播放一个指定频率、时长的正弦波音（带淡入淡出，避免 click 爆音）
void playTone(float freqHz, int durationMs, float volume = 0.3f) {
  const int totalSamples = (int)(SAMPLE_RATE * durationMs / 1000.0);
  const int fade = SAMPLE_RATE / 200;               // 5ms 淡入淡出
  int16_t buf[256];
  int idx = 0;

  for (int n = 0; n < totalSamples; n++) {
    float amp = volume;
    if (n < fade)                amp *= (float)n / fade;              // 淡入
    if (n > totalSamples - fade) amp *= (float)(totalSamples - n) / fade;  // 淡出

    float s = sinf(2.0f * M_PI * freqHz * n / SAMPLE_RATE);
    buf[idx++] = (int16_t)(s * amp * 32767);

    if (idx == 256) {
      size_t written;
      i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
      idx = 0;
    }
  }
  if (idx > 0) {
    size_t written;
    i2s_write(I2S_PORT, buf, idx * sizeof(int16_t), &written, portMAX_DELAY);
  }
}

// 播放编译进固件的语音 PCM。
// 注意：voicePCM 在 flash（.rodata），而 I2S DMA 只能从 RAM 读——直接把 flash 地址
// 喂给 i2s_write 会传出垃圾/无声。所以分块先 memcpy 到 RAM 栈缓冲再写。
void playVoice(float gain = 0.6f) {
  int16_t ramBuf[512];                       // 1KB RAM 缓冲
  size_t i = 0;
  while (i < VOICE_NUM_SAMPLES) {
    size_t chunk = VOICE_NUM_SAMPLES - i;
    if (chunk > 512) chunk = 512;
    // 边拷边按 gain 缩放：降低瞬时电流峰值，缓解供电塌陷 / 削顶失真
    for (size_t k = 0; k < chunk; k++)
      ramBuf[k] = (int16_t)(voicePCM[i + k] * gain);
    size_t written = 0;
    i2s_write(I2S_PORT, ramBuf, chunk * sizeof(int16_t), &written, portMAX_DELAY);
    i += chunk;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Demo16 喇叭播放测试 ===");
  initSpeaker();
}

// 硬件自检：连续输出不间断的 440Hz 中音。不碰 flash 语音数据，排除软件变量。
// 听感判断：
//   平稳"嘟——"不断不抖 → 焊点好、供电稳（硬件 OK，问题在语音数据/音量）
//   断断续续/忽有忽无    → 虚焊（拨焊点音会变）
//   周期中断/音调发抖    → 供电塌陷
void loop() {
  Serial.println("播放语音：你好，我是小esp32，喇叭测试成功啦");
  playVoice(1.0f);                  // 数据已归一化，不再二次缩放（避免量化噪声）

  // 播完立刻清零 DMA 缓冲，避免空闲期功放持续放大底噪（沙沙声）
  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("一轮结束，3 秒后重播\n");
  delay(3000);
}
