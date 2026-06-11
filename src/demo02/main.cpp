// Demo 2 — 串口对话
// 目标：板子每秒往电脑打印文字，同时 LED 继续闪
// 知识点：Serial.begin / Serial.println，串口监视器
// 提示：烧录后用 `pio device monitor` 看输出（波特率 115200，要和代码一致）

#include <Arduino.h>

void setup() {
  Serial.begin(115200);          // 开启串口，波特率 115200
  delay(1000);                   // 等串口稳定 + 给你时间连上 monitor，否则开头几行 log 易丢失
  Serial.println("Hello, ESP32!");
  Serial.println("This is a simple LED blink example.");
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_BUILTIN, LOW);  // 低电平 -> 灯亮
  Serial.println("LED is ON");
  delay(1000);
  digitalWrite(LED_BUILTIN, HIGH); // 高电平 -> 灯灭
  Serial.println("LED is OFF");
  delay(1000);
}
