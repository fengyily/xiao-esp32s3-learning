#include <Arduino.h>

#define LED_PIN LED_BUILTIN
#define PWM_CHANNEL 0
#define PWM_FREQ 5000
#define PWM_RESOLUTION 8

unsigned long times = 0; // 

void setup() {
  Serial.begin(115200);
  
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION); // 配置 PWM 通道
  ledcAttachPin(LED_PIN, PWM_CHANNEL); // 将 LED 引脚连接到 PWM 通道
}

void loop() {
  // put your main code here, to run repeatedly:
  for (int b = 0; b <=255; b++) {
    ledcWrite(PWM_CHANNEL, b); // 设置 PWM 占空比，范围 0-255
    delay(10); // 等 10ms，调整亮度变化速度
  }

  for (int b = 255; b >=0; b--) {
    ledcWrite(PWM_CHANNEL, b); // 设置 PWM 占空比，范围 0-255
    delay(10); // 等 10ms，调整亮度变化速度
  }
  Serial.println("Brightness cycle complete, times " + String(++times));
}
