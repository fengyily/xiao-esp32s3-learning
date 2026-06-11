module github.com/fengyily/xiao-esp32s3-learning/server

go 1.24

// 纯标准库实现，无第三方依赖：
// - 阿里云 POP 签名用 crypto/hmac + crypto/sha1
// - HTTP 服务用 net/http
