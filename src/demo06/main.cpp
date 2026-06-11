// Demo 6 — 摄像头拍照存 SD 卡（SPI 方式）
// 目标：按一下按钮(D1↔GND)拍一张照片，存进 SD 卡，文件名递增 /pic_1.jpg、/pic_2.jpg ...
// 知识点：摄像头初始化(camera_config_t)、esp_camera_fb_get 抓帧、SD(SPI) 写文件、PSRAM
//
// 关于 SD 卡（重要更正）：
//   最初本 Demo 用 SDMMC 接口(SD_MMC.setPins/begin)挂载，一直 0x107 超时。
//   后来 Demo 100 用 SPI 方式(SD.begin(CS=21))成功 → 证明卡和硬件都好，
//   是当初用错了接口/引脚。本版改用 Demo 100 验证过的 SPI 方式。
//   SPI 默认引脚 SCK=7/MISO=8/MOSI=9，CS=21（=板载 LED，挂载时灯会闪，正常）。
//
// 硬件前提：
//   1. Sense 扩展板装好、摄像头排线(FPC)插紧
//   2. 插一张 FAT32 的 SD 卡
//   3. 按钮：金属短接 D1(GPIO2) ↔ GND（沿用 Demo 3 的接法）
//
// platformio.ini 里 demo06 已开 PSRAM（-DBOARD_HAS_PSRAM + qio_opi），摄像头帧缓冲要用它。

#include <Arduino.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

#define SD_CS_PIN 21               // Sense 板 SD 卡片选（走 SPI），和 Demo 100 一致

// ---------- XIAO ESP32-S3 Sense 摄像头引脚（官方定义，勿改）----------
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

#define BTN_PIN D1                 // 按钮：D1(GPIO2) ↔ GND
const unsigned long DEBOUNCE_MS = 50;

int  lastReading = HIGH;
int  stableState = HIGH;
unsigned long lastDebounceTime = 0;

int  picIndex = 0;                 // 照片序号，每拍一张 +1

// ---------- 摄像头初始化 ----------
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;             // 20MHz 主时钟
  config.frame_size   = FRAMESIZE_UXGA;       // 1600x1200：存卡不怕大，画质高
  config.pixel_format = PIXFORMAT_JPEG;       // 直接出 JPEG，省内存、方便存文件
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;   // 帧缓冲放 PSRAM
  config.jpeg_quality = 12;                   // 数字越小画质越高(0~63)
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败, 错误码 0x%x\n", err);
    return false;
  }
  return true;
}

// ---------- 拍一张并存到 SD 卡（SPI）----------
void takePhoto() {
  // 丢掉前几帧（刚触发时曝光可能没稳，第一帧偏暗/发绿）
  for (int i = 0; i < 3; i++) {
    camera_fb_t *warm = esp_camera_fb_get();
    if (warm) esp_camera_fb_return(warm);     // 用完立刻归还，否则内存泄漏
  }

  camera_fb_t *fb = esp_camera_fb_get();      // 真正要保存的这一帧
  if (!fb) {
    Serial.println("抓帧失败");
    return;
  }

  picIndex++;
  String path = "/pic_" + String(picIndex) + ".jpg";

  File file = SD.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.printf("打开文件失败: %s\n", path.c_str());
    esp_camera_fb_return(fb);                 // 出错也要归还
    return;
  }

  file.write(fb->buf, fb->len);               // fb->buf=JPEG字节, fb->len=长度
  file.close();
  Serial.printf("已保存 %s, 大小 %u 字节\n", path.c_str(), fb->len);

  esp_camera_fb_return(fb);                   // 关键：归还帧缓冲
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Demo 6: 摄像头拍照存 SD 卡（SPI 方式）===");

  pinMode(BTN_PIN, INPUT_PULLUP);

  if (!initCamera()) {
    Serial.println("摄像头挂了，检查排线是否插紧。停止。");
    while (true) delay(1000);
  }
  Serial.println("摄像头 OK");

  // SPI 方式挂载 SD 卡（CS=21），和 Demo 100 验证过的一致
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD 挂载失败：检查卡是否插好/是否 FAT32。停止。");
    while (true) delay(1000);
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("没检测到 SD 卡。停止。");
    while (true) delay(1000);
  }
  Serial.printf("SD 卡 OK，容量 %lluMB\n", SD.cardSize() / (1024 * 1024));

  Serial.println("准备就绪：短接 D1↔GND 拍一张照片，存到 SD 卡");
}

void loop() {
  // 复用 Demo 3 的按钮消抖逻辑：下降沿(按下)那一刻拍照
  int reading = digitalRead(BTN_PIN);
  if (reading != lastReading) lastDebounceTime = millis();

  if (millis() - lastDebounceTime > DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {     // 按下 -> 拍照
        Serial.println("按下，拍照中...");
        takePhoto();
      }
    }
  }
  lastReading = reading;
}
