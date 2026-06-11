#include <Arduino.h>

#define BTN_PIN D1   // 按钮接 D1(=GPIO2)；另一头接 GND。避开 GPIO0 的启动复用
const unsigned long DEBOUNCE_MS = 50; // 消抖时间，单位毫秒

int lastReading = HIGH; // 上次读取的按钮状态，初始为未按下（HIGH）
int stableState = HIGH; // 稳定的按钮状态，初始为未按下（HIGH）
unsigned long lastDebounceTime = 0; // 上次状态变化的时间
unsigned long pressCount = 0; // 按钮按下的次数
bool ledOn = false; // LED状态
unsigned long lastPrintTime = 0; // 上次打印状态的时间

void setup() {
  Serial.begin(115200);          // 开启串口，波特率 115200
  delay(1000);                   // 等串口稳定 + 给你时间连上 monitor，否则开头几行 log 易丢失
  Serial.println("=== Demo 3: 按钮输入 ===");
  Serial.println("This is a button debounce example.");
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP); // 设置按钮引脚为输入，启用内部上拉电阻
}

void loop() {
  int reading = digitalRead(BTN_PIN);  //	读当前原始电平（HIGH/LOW）
  if (millis() - lastPrintTime > 500) { // 每500ms打印一次状态
    Serial.print("D1 = ");
    Serial.print(reading);          // 平时 1(HIGH)，碰到 GND 应变 0(LOW)
    Serial.print(", count = ");
    Serial.println(pressCount);
    lastPrintTime = millis();
  }

  if (reading != lastReading) { // 状态发生变化，重置计时器
    lastDebounceTime = millis();
  }

  if (millis() - lastDebounceTime > DEBOUNCE_MS) { // 如果状态稳定超过消抖时间
    if (reading != stableState) { // 状态发生改变
      stableState = reading; // 更新稳定状态
      if (stableState == LOW) { // 按钮被按下
        pressCount++; // 增加按下次数
        ledOn = !ledOn; // 切换LED状态
        digitalWrite(LED_BUILTIN, ledOn ? LOW : HIGH); // 根据LED状态控制灯亮灭
      }
    }
  }
  lastReading = reading; // 更新上次读取状态
}