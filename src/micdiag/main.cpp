// 麦克风采样率诊断 —— 实测 I2S/PDM 实际产出多少采样/秒
// 目的：之前录音"听起来变速"，怀疑实际采样率 ≠ 设定的 16000。
//       这里连续读 5 秒墙上时间，数实际收到多少采样，反推真实采样率。
//
// 不上传、不识别，纯本地测量。

#include <Arduino.h>
#include <I2S.h>

#define MIC_CLK   42
#define MIC_DATA  41
#define SET_RATE  32000        // 试 32000：上次设16000实测仅8339，约一半；
                               // 若设32000能实测出~16000，就用"设32k按16k处理"拿到真16k音质

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== 麦克风采样率诊断 ===");
  Serial.printf("设定采样率 = %d Hz，下面实测实际产出\n", SET_RATE);

  I2S.setAllPins(-1, MIC_CLK, MIC_DATA, -1, -1);
  if (I2S.begin(PDM_MONO_MODE, SET_RATE, 16) == 0) {
    Serial.println("I2S 初始化失败");
    while (true) delay(1000);
  }
  Serial.println("麦克风 OK，开始测量…");
}

void loop() {
  // 读丢前几批，跳过启动杂音
  static bool warmed = false;
  if (!warmed) {
    uint8_t w[2048];
    for (int i = 0; i < 8; i++) I2S.read(w, sizeof(w));
    warmed = true;
  }

  // 连续读 5 秒墙上时间，累计读到多少字节
  const unsigned long MEASURE_MS = 5000;
  uint8_t buf[4096];
  unsigned long start = millis();
  unsigned long totalBytes = 0;

  while (millis() - start < MEASURE_MS) {
    int n = I2S.read(buf, sizeof(buf));
    if (n > 0) totalBytes += n;
  }
  unsigned long elapsed = millis() - start;

  // 16bit = 2 字节/采样
  unsigned long samples = totalBytes / 2;
  float actualRate = samples * 1000.0 / elapsed;

  Serial.println("----------------------------------------");
  Serial.printf("墙上时间   : %lu ms\n", elapsed);
  Serial.printf("读到字节   : %lu\n", totalBytes);
  Serial.printf("读到采样   : %lu\n", samples);
  Serial.printf("==> 实际采样率 ≈ %.0f Hz （设定 %d）\n", actualRate, SET_RATE);
  if (actualRate > SET_RATE * 1.2) {
    Serial.println("实际 > 设定：数据被当低采样率存会听起来变慢；反之变快。");
  }
  Serial.println("----------------------------------------\n");
  delay(2000);
}
