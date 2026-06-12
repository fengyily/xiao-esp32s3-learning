# XIAO ESP32-S3 Sense 学习计划

> 从 0 基础到边缘 AI，通过一个个能跑起来、看得见效果的 Demo 循序渐进。
> 每完成一个 demo，把 `[ ]` 改成 `[x]` 打勾。

---

## 认识你的硬件

**XIAO ESP32-S3 Sense** —— 指甲盖大小但功能很全：

- **ESP32-S3 双核芯片** —— 算力强，支持 WiFi + 蓝牙(BLE)
- **8MB PSRAM + 8MB Flash** —— 内存大，能跑 AI / 摄像头
- **OV2640 摄像头** —— Sense 版本特有，能拍照、做图像识别
- **数字麦克风 (PDM MIC)** —— 能录音、做语音
- **SD 卡槽** —— 能存照片、录音、日志
- **板载橙色 LED** —— 接在 **GPIO 21**，且是 **低电平点亮**（写 `LOW` 才亮，这是常见的坑）

这块板子最大的价值是 **摄像头 + 麦克风 + AI**，学习路线最终会走向"边缘 AI"。

---

## 工具链速查

| 操作 | 命令 |
|---|---|
| 编译 | `pio run` |
| 烧录到板子 | `pio run -t upload` |
| 看串口输出 | `pio device monitor` |
| 编译+烧录+看串口 | `pio run -t upload -t monitor` |

> 如果提示 `pio: command not found`，说明 PATH 没配。把这行加到 `~/.zshrc`：
> `export PATH="$PATH:$HOME/.platformio/penv/bin"`

---

## 学习路线（10 个 Demo，4 个阶段）

### 阶段一：基础 —— 跑通工具链，掌握输入输出

- [x] **Demo 1 — Blink 闪灯** ✅ 完成
  - 学：烧录流程、GPIO、`setup()`/`loop()` 结构、`pinMode` / `digitalWrite` / `delay`
  - 硬件：板载 LED (GPIO 21，低电平点亮)
  - 目标：LED 每秒闪一次

- [x] **Demo 2 — 串口对话** ✅ 完成
  - 学：`Serial.begin` / `Serial.println`，串口监视器 —— 以后最常用的调试工具
  - 硬件：USB 串口
  - 目标：板子每秒往电脑打印一行文字

- [x] **Demo 3 — 按钮输入** ✅ 完成
  - 学：`pinMode(INPUT_PULLUP)`、读引脚、按键消抖、下降沿检测
  - 硬件：用金属短接 D1(GPIO2) ↔ GND 模拟按钮（手头无现成开关）
  - 目标：按一下按钮，串口打印一次 / 切换 LED

### 阶段二：传感与外设 —— 用上这块板的特色硬件

- [x] **Demo 4 — PWM 呼吸灯** ✅ 完成
  - 学：模拟输出、`ledcSetup`/`ledcAttachPin`/`ledcWrite`，占空比
  - 硬件：板载 LED
  - 目标：LED 由暗到亮再到暗，呼吸效果

- [x] **Demo 5 — 读麦克风音量** ✅ 完成
  - 学：I2S / PDM 数字麦克风采集（CLK=42, DATA=41），把音量画成串口音量条
  - 硬件：板载麦克风
  - 目标：对着板子说话，串口音量条跟着跳

- [x] **Demo 6 — 摄像头拍照存 SD 卡** ✅ 完成（SPI 方式，真·达成原目标）
  - 学：摄像头初始化(camera_config_t)、`esp_camera_fb_get`/`fb_return`、PSRAM、`SD.begin(CS)` SPI 存文件
  - 硬件：摄像头 + SD 卡（SPI：默认 SCK=7/MISO=8/MOSI=9 + CS=21）
  - 目标：按 D1↔GND 拍一张，UXGA(1600x1200) JPEG 存进 SD 卡，文件名递增 /pic_N.jpg
  - 备注：最初用 SDMMC 挂载一直 0x107 超时，误判成硬件坏；后来 Demo 100 用 SPI 跑通，
    证明是接口选错。已改回 SPI，连拍 4 张成功(~100KB/张)。另保留了 base64 串口 dump 版的思路(tools/recv_photo.py)。

### 阶段三：联网 —— 让板子上网

- [x] **Demo 7 — 连 WiFi + 获取网络时间** ✅ 完成
  - 学：`WiFi.begin`/`WiFi.status` 轮询、`configTime` NTP 对时、`getLocalTime`+struct tm、secrets.h 隔离密码
  - 硬件：WiFi（2.4G）
  - 目标：连上家里 WiFi，串口打印当前准确时间

- [x] **Demo 8 — 网页看摄像头实时画面** ✅ 完成
  - 学：`WebServer` 路由、MJPEG(multipart/x-mixed-replace) 视频流、摄像头+WiFi 合体、secrets.h 跨 demo 复用
  - 硬件：WiFi + 摄像头（不用 SD 卡）
  - 目标：手机/电脑浏览器输入板子 IP，看到实时画面

### 附加：硬件排查 —— SD 卡那一路

- [x] **Demo 100 — SD 卡加载测试** ✅ 完成（基于官方 SD 示例改写）
  - 学：`fs::FS` 文件系统抽象、`SD.begin(CS)` SPI 挂载、`File` 读写/改名/删除、IO 跑分
  - 硬件：Sense 板 SD 卡槽（CS=GPIO21，走 SPI；卡需 FAT32）
  - 目标：串口打印卡类型/容量，跑一轮读写并测速；用来排查 Demo 6 卡住的 0x107
  - 备注：这是 **SPI 方式**（`SD.h` + CS），跟 Demo 6 失败时用的 **SDMMC 方式**（`SD_MMC.setPins`）不是一条路，可对照看哪条通。

### 阶段四：进阶 / 边缘 AI —— 这块板的终极玩法

- [ ] **Demo 9 — 声控触发**
  - 学：麦克风音量阈值检测，超过阈值触发动作
  - 硬件：麦克风
  - 目标：拍手/喊一声，LED 亮起或拍照

- [ ] **Demo 10 — 边缘图像识别**
  - 学：在板子上跑一个轻量模型（如 Edge Impulse / TensorFlow Lite Micro）做物体识别
  - 硬件：摄像头 + AI
  - 目标：摄像头对着物体，识别出是什么并打印结果

---

## 进度日志

| 日期 | Demo | 备注 |
|---|---|---|
| 2026-06-10 | Demo 1 | ✅ 完成：编写 Blink，编译烧录成功，LED 每秒闪一次 |
| 2026-06-10 | Demo 2 | ✅ 完成：串口每秒打印，LED 同步闪 |
| 2026-06-10 | Demo 3 | ✅ 完成：D1↔GND 短接模拟按钮，消抖+下降沿正确，count 不乱跳，LED 翻转 |
| 2026-06-10 | Demo 4 | ✅ 完成：LEDC PWM 呼吸灯，板载 LED 平滑呼吸 |
| 2026-06-10 | Demo 5 | ✅ 完成：PDM 麦克风音量条，实测 level 定 map 下限消底噪，说话/拍手条变长 |
| 2026-06-11 | Demo 6 | ✅ 完成：OV2640 拍照；先 base64 串口 dump，后查明 0x107 是接口选错(应走 SPI)，改 SD.begin(21) SPI 存卡成功，连拍 4 张 ~100KB/张 |
| 2026-06-11 | Demo 7 | ✅ 完成：连 WiFi(2.4G)+NTP 对时，每秒打印准确时间；密码隔离进 secrets.h(gitignore) |
| 2026-06-11 | Demo 8 | ✅ 完成：板子当 Web 服务器，浏览器看 MJPEG 实时画面，流畅；复用 demo07 的 secrets.h |
| 2026-06-11 | Demo 100 | ✅ 完成：SPI 方式 SD 卡(CS=21)挂载成功，读写测速通过(1MB 读 2398ms/写 2664ms)，借此查明 Demo 6 的 0x107 真因 |
| 2026-06-11 | Demo 11 | ✅ 完成：语音转文字！板子录音 3s→上传 Go 中转服务→阿里云识别→串口显示文字。对话功能第 1-2 环打通 |
| 2026-06-12 | Demo 11 | ✅ 升级：声控触发+DeepSeek 对话+原生 PDM(修录音又闷又糊)+VAD+预缓冲，免提语音助手成型 |
| 2026-06-12 | Demo 12 | ✅ 完成：实时中英互译，说话→识别→DeepSeek 翻译→打印译文(后续接喇叭播报) |
| 2026-06-12 | Demo 13 | ✅ 完成：全双工对话，FreeRTOS 双任务边听边处理(半句不丢)+会话上下文记忆 |
| 2026-06-12 | Demo 14 | ✅ 完成：全双工流式翻译，双任务+DeepSeek 流式输出(译文打字机式)，一句一译 |

---

## 我学到的坑（随手记）

- XIAO ESP32-S3 板载 LED 是**低电平点亮**：`digitalWrite(LED_BUILTIN, LOW)` 才是亮。
- `HIGH` / `LOW` 是常量（值），不是函数；写引脚要用 `digitalWrite(引脚, HIGH/LOW)`。
- `pio` 命令需要 PATH 里有 `~/.platformio/penv/bin`。
- **按钮永远只短接 2 个点**：信号脚（如 D1）+ GND。碰到第 3 个点（电源/复位）会重启甚至伤板。
- **背面 (B) 焊盘 = GPIO 0 = BOOT**，它下面那个无字焊盘不是 GND；碰它会让板子复位（串口从头打印、count 归零）。做按钮要用边缘明确标 `GND` 的脚。
- **GPIO 0 有启动复用**：上电/复位瞬间被拉低会进烧录模式，所以做普通输入优先选别的脚（这里用了 D1=GPIO2）。
- `pinMode(pin, INPUT_PULLUP)` 是给 `pinMode` 用的，不是 `digitalRead`；`digitalRead(pin)` 只传引脚号，且要用变量接住返回值。
- 引脚名 `D0/D1/...` 板子已定义好（`D0=GPIO1, D1=GPIO2`），代码里直接写 `D1` 比写数字更清楚。
- **ESP32 的 PWM 不用 `analogWrite`**，用 LEDC 三步：`ledcSetup(通道,频率,分辨率)` → `ledcAttachPin(引脚,通道)` → `ledcWrite(通道,duty)`。
- **PWM 呼吸灯也踩低电平点亮的坑**：想越亮 duty 要越小，写 `255 - b` 才不会亮暗反向。
- **PDM 麦克风引脚硬连死**：CLK=GPIO42, DATA=GPIO41，板载不用接线；`I2S.read()` 无"有效"标志，出错只返回 0。
- **传感器映射先实测再定参数**：麦克风有底噪/直流偏置，安静时 `level` 不为 0。先 `Serial.println(level)` 看安静值和说话峰值，再用它们当 `map(level, 安静值, 峰值, 0, 30)` 的上下限，别套示例数字。
- **摄像头要开 PSRAM**：platformio.ini 里 `-DBOARD_HAS_PSRAM` + `board_build.arduino.memory_type = qio_opi`，否则大帧缓冲挂掉；`esp_camera_fb_get()` 抓帧后必须 `esp_camera_fb_return()` 归还，否则连拍几张就内存爆。
- **⚠️【已更正】Demo 6 的 0x107 不是硬件问题，是接口选错**：当初用 SDMMC(`SD_MMC.setPins/begin`)挂不上一直超时 0x107，误判成"硬件接触不通"。后来 Demo 100 用 **SPI 方式 `SD.begin(21)` 成功**，证明卡和卡槽都好。真因：这块板 SD 卡该走 **SPI**(默认 SCK=7/MISO=8/MOSI=9 + CS=21)，不是 SDMMC。Demo 6 已改回 SPI 方式存卡成功。教训：挂载失败先怀疑**接口/引脚选型**，别急着下"硬件坏了"的结论。
- **SD 卡有两条路**：`SD_MMC`（SDMMC 接口，Demo 6 用过，0x107 卡住）和 `SD.h`+`SD.begin(CS)`（SPI 接口，Demo 100 用，CS=GPIO21）。两条路底层不同，一条不通可换另一条试。SPI 方式更通用但更慢；`SD.begin()` 不传 CS 默认用 SS 脚，本板要显式传 21。
- **板子没法存文件时，可串口 dump**：把数据 base64 编码、用标记行 `---BEGIN---/---END---` 包起来打印，电脑端脚本(`tools/recv_photo.py`)解码还原。分块编码(每块 240 字节=3 的倍数)避免内存爆 + base64 错位。
- **ESP32-S3 只支持 2.4G WiFi**，连不上先确认连的不是 5G 频段；`WiFi.begin` 后要轮询 `WiFi.status()`，记得加超时别死等。
- **struct tm 的坑**：`tm_year` 从 1900 起算(要 +1900)、`tm_mon` 是 0~11(要 +1)，否则打出 0124 年。NTP `configTime` 是异步的，头几秒 `getLocalTime` 可能还没同步好，要容错。
- **敏感信息隔离**：WiFi 密码等放进 `secrets.h`，在 .gitignore 里忽略 `secrets.h` / `src/**/secrets.h`，代码可公开、密码留本地。
- **MJPEG 视频流原理**：其实就是不停发一张张 JPEG。响应头 `Content-Type: multipart/x-mixed-replace; boundary=frame`，然后循环发 `--frame` + 每帧的 `Content-Type/Content-Length` + JPEG 字节。boundary 名字前后要一致（头里 `frame`、循环里 `--frame`）。流循环里同样要 `esp_camera_fb_return` 否则内存爆。
- **流模式摄像头参数**：`grab_mode=CAMERA_GRAB_LATEST`(取最新帧降延迟) + `fb_count=2`(双缓冲更顺)；分辨率先用 VGA，太大帧率掉。
- **跨 demo 复用头文件**：platformio.ini 里用 `build_flags = -I src/demo07` 就能让 demo08 直接 `#include "secrets.h"`，密码只维护一份。
