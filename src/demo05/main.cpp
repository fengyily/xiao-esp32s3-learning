#include <Arduino.h>
#include <I2S.h>

#define MIC_CLK 42
#define MIC_DATA 41


void setup() {
  Serial.begin(115200);
  Serial.println("Hello, ESP32S3!");
  I2S.setAllPins(-1, MIC_CLK, MIC_DATA, -1, -1);
  if (I2S.begin(PDM_MONO_MODE, 16000, 16) == 0) {
    Serial.println("Failed to initialize I2S!");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("I2S initialized successfully!");
}

void loop() {
    long sum = 0;
    int N = 0;
    for (int i = 0; i < 256; i++) {
        int sample = I2S.read();
        sum += abs(sample);
        N++;
    }

    int level = sum / N; 

    int bars = map(level, 1200, 10000, 0, 50); // 根据实际情况调整最大值
    bars = constrain(bars, 0, 50); // 限制在0-30范围内
    //Serial.println(bars);

    Serial.print("[");
    for (int i = 0; i < bars; i++) {
        Serial.print("#"); 
    }
    Serial.println("]");
}