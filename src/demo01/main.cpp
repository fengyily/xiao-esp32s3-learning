// Demo 1 — Blink 闪灯
// 目标：板载 LED 每秒亮灭一次
// 知识点：setup/loop 结构、pinMode / digitalWrite / delay
// 硬件坑：XIAO ESP32-S3 板载 LED 在 GPIO 21，且低电平点亮（LOW = 亮）

#include <Arduino.h>

void setup() {
  pinMode(LED_BUILTIN, OUTPUT); // 把 LED 引脚设为输出模式
}

void loop() {
  digitalWrite(LED_BUILTIN, LOW);  // 低电平 -> 灯亮
  delay(1000);                     // 等 1 秒
  digitalWrite(LED_BUILTIN, HIGH); // 高电平 -> 灯灭
  delay(1000);                     // 等 1 秒
}
