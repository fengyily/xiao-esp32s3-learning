// GPIO2(D1) 损坏诊断 —— 用数字/ADC/触摸三种方式读，对比好脚 D2(GPIO3)
// 目的：搞清 D1 是整个脚坏了，还是只有数字输入坏。零接线、零风险。
//
// 怎么看：
//   悬空(什么都不接)时——
//     数字 INPUT_PULLUP：好脚应=1。D1 若=0 → 数字上拉/输入异常
//     ADC analogRead    ：悬空值会飘，但好脚和坏脚应都能读出"会变化的数"
//                         若 D1 恒为 0 或恒为满量程且纹丝不动 → ADC 也坏
//     touchRead         ：好脚有一个基线值(几百~几千)；D1 若=0 或异常 → 触摸也坏
//   结论：
//     三种全异常 → 整个 GPIO2 物理损坏
//     只有数字异常、ADC/触摸正常 → 仅数字输入坏（脚还半活）

#include <Arduino.h>

#define BAD_PIN   2    // D1 = GPIO2（怀疑坏的）
#define GOOD_PIN  3    // D2 = GPIO3（参照，应正常）

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== GPIO2(D1) 诊断：数字/ADC/触摸 三方式 ===");
  Serial.println("悬空测试，什么都别接。对比 GPIO2(坏?) vs GPIO3(好)");
}

void loop() {
  // 1. 数字 INPUT_PULLUP
  pinMode(BAD_PIN, INPUT_PULLUP);
  pinMode(GOOD_PIN, INPUT_PULLUP);
  delay(5);
  int dBad  = digitalRead(BAD_PIN);
  int dGood = digitalRead(GOOD_PIN);

  // 2. ADC
  int aBad  = analogRead(BAD_PIN);
  int aGood = analogRead(GOOD_PIN);

  // 3. 触摸
  long tBad  = touchRead(BAD_PIN);
  long tGood = touchRead(GOOD_PIN);

  Serial.printf("数字: GPIO2=%d GPIO3=%d | ADC: GPIO2=%4d GPIO3=%4d | 触摸: GPIO2=%ld GPIO3=%ld\n",
                dBad, dGood, aBad, aGood, tBad, tGood);
  delay(800);
}
