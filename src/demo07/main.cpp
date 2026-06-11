// Demo 7 — 连 WiFi + 获取网络时间(NTP)
// 目标：连上家里 WiFi -> 打印 IP -> NTP 对时 -> 每秒打印当前准确时间
// 知识点：WiFi.begin / WiFi.status 轮询、configTime NTP 对时、getLocalTime + struct tm
//
// 硬件：不接线，纯软件用板载 WiFi
// 注意：ESP32-S3 只支持 2.4G WiFi；账号密码填在 secrets.h（已被 gitignore，不会上传）

#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"          // 提供 WIFI_SSID / WIFI_PASSWORD

// 中国东八区：UTC+8 = 8*3600 秒；不用夏令时，第二个参数 0
const long  GMT_OFFSET_SEC      = 8 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

void connectWiFi() {
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);                 // station 模式（连别人的路由器）
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // 轮询连接状态，最多等 ~20 秒，避免密码错时死等
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("已连接！IP = ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("连接失败：检查 secrets.h 里的账号密码，确认连的是 2.4G。停止。");
    while (true) delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Demo 7: 连 WiFi + 网络对时 ===");

  connectWiFi();

  // 发起 NTP 对时（用国内服务器更快更稳）。这步是异步的，要等几秒才同步好。
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "ntp.aliyun.com", "pool.ntp.org");
  Serial.println("已发起 NTP 对时，等待同步…");
}

void loop() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {       // 取本地时间，成功才打印
    // ⚠️ tm_year 是从 1900 起算 -> +1900；tm_mon 是 0~11 -> +1
    Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_mday,
                  timeinfo.tm_hour,
                  timeinfo.tm_min,
                  timeinfo.tm_sec);
  } else {
    Serial.println("还没拿到时间，等待 NTP 同步…");
  }
  delay(1000);
}
