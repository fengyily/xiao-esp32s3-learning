// Demo 8 — 网页看摄像头实时画面（MJPEG 流）
// 目标：板子当 Web 服务器，手机/电脑浏览器输入板子 IP，看到摄像头实时画面
// 知识点：WebServer 路由、MJPEG(multipart/x-mixed-replace) 视频流、摄像头+WiFi 合体
//
// 这是 Demo 6(摄像头) + Demo 7(WiFi) 的合体，不需要 SD 卡。
// 硬件：摄像头 + WiFi(2.4G)，不接线。WiFi 账号密码复用 demo07/secrets.h。
//
// platformio.ini 里 demo08 已开 PSRAM，并用 -I src/demo07 引入 secrets.h。

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h"          // 来自 src/demo07/secrets.h（-I 引入）

// ---------- XIAO ESP32-S3 Sense 摄像头引脚（官方定义，和 Demo 6 一致）----------
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

WebServer server(80);

// ---------- 摄像头初始化（和 Demo 6 一样，分辨率调小让流更流畅）----------
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
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_VGA;        // 640x480，流畅；想更清晰改 SVGA/XGA
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;   // 流模式取最新帧，延迟更低
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 2;                     // 双缓冲，流更顺

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败, 错误码 0x%x\n", err);
    return false;
  }
  return true;
}

// ---------- 首页：一个 <img> 指向 /stream ----------
void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>XIAO Cam</title></head>"
    "<body style='margin:0;background:#111;text-align:center'>"
    "<h2 style='color:#eee'>XIAO ESP32-S3 实时画面</h2>"
    "<img src='/stream' style='max-width:100%;height:auto'>"
    "</body></html>";
  server.send(200, "text/html", html);
}

// ---------- MJPEG 流：核心 ----------
// 用 multipart/x-mixed-replace，不断发一张张 JPEG，浏览器连起来当视频显示。
void handleStream() {
  WiFiClient client = server.client();

  // 1. 先发 multipart 响应头，声明 boundary=frame
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("\r\n");

  // 2. 死循环：抓帧 -> 发帧，直到浏览器断开
  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("抓帧失败");
      break;
    }

    // 每帧前发一段 multipart 头：分隔符 + 类型 + 长度
    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.printf("Content-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);            // 这帧的 JPEG 字节
    client.print("\r\n");

    esp_camera_fb_return(fb);                   // 关键：归还，否则内存爆、流卡死

    if (!client.connected()) break;             // 浏览器关了就退出
  }
}

void connectWiFi() {
  Serial.printf("连接 WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 连接失败：检查 secrets.h，确认连的是 2.4G。停止。");
    while (true) delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Demo 8: 网页看摄像头实时画面 ===");

  if (!initCamera()) {
    Serial.println("摄像头挂了，检查排线。停止。");
    while (true) delay(1000);
  }
  Serial.println("摄像头 OK");

  connectWiFi();
  Serial.print("已连接！在浏览器打开： http://");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/stream", handleStream);
  server.begin();
  Serial.println("Web 服务器已启动。手机/电脑连同一个 WiFi，访问上面的地址。");
}

void loop() {
  server.handleClient();    // 处理浏览器请求
}
