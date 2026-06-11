// Demo 6 — 摄像头拍照（串口 dump 版）
// 目标：按一下按钮(D1↔GND)拍一张照片，把 JPEG 经 base64 打印到串口，
//       电脑端用脚本(tools/recv_photo.py)还原成 .jpg 查看。
// 知识点：摄像头初始化(camera_config_t)、esp_camera_fb_get 抓帧、base64、PSRAM
//
// 背景：这块板子 SD 卡那一路物理接触不通(0x107 超时)，所以不走 SD，改串口 dump。
//       Demo 8(网页看摄像头)本来也不用 SD，不影响后续学习。
//
// 硬件前提：
//   1. Sense 扩展板装好、摄像头排线(FPC)插紧
//   2. 按钮：金属短接 D1(GPIO2) ↔ GND（沿用 Demo 3 的接法）
//
// platformio.ini 里 demo06 已开 PSRAM（-DBOARD_HAS_PSRAM + qio_opi），摄像头帧缓冲要用它。

#include <Arduino.h>
#include "esp_camera.h"
#include "base64.h"

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
  config.frame_size   = FRAMESIZE_SVGA;       // 800x600：串口 dump 用，体积小传输快
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

// ---------- 拍一张并经串口 dump 出去 ----------
// 输出格式（电脑端脚本靠这几行标记来识别）：
//   ---PHOTO-BEGIN--- <序号> <字节数>
//   <base64 数据，多行，每行 360 字符>
//   ---PHOTO-END---
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
  Serial.printf("---PHOTO-BEGIN--- %d %u\n", picIndex, fb->len);

  // 分块 base64 编码：每次取 240 字节原始数据(=320 个 base64 字符，整除避免错位)，
  // 整张一次性编码会吃太多内存。240 是 3 的倍数，保证每块独立编码不串位。
  const size_t CHUNK = 240;
  for (size_t off = 0; off < fb->len; off += CHUNK) {
    size_t n = (fb->len - off < CHUNK) ? (fb->len - off) : CHUNK;
    Serial.println(base64::encode(fb->buf + off, n));
  }

  Serial.println("---PHOTO-END---");
  Serial.printf("完成：第 %d 张，JPEG %u 字节\n", picIndex, fb->len);

  esp_camera_fb_return(fb);                   // 关键：归还帧缓冲
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Demo 6: 摄像头拍照（串口 dump 版）===");

  pinMode(BTN_PIN, INPUT_PULLUP);

  if (!initCamera()) {
    Serial.println("摄像头挂了，检查排线是否插紧。停止。");
    while (true) delay(1000);
  }
  Serial.println("摄像头 OK");

  Serial.println("准备就绪：短接 D1↔GND 拍一张照片（用 tools/recv_photo.py 接收）");
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
