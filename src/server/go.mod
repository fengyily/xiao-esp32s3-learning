module github.com/fengyily/xiao-esp32s3-learning/server

go 1.24

// 基本用标准库：
// - 阿里云 POP 签名用 crypto/hmac + crypto/sha1
// - HTTP 服务用 net/http
// 唯一第三方依赖：gorilla/websocket（阿里云实时识别 WS 客户端，Demo 15）

require github.com/gorilla/websocket v1.5.3
