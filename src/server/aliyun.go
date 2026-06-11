package main

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"sync"
	"time"
)

// AliyunASR 封装阿里云一句话识别：负责换 Token（带缓存）+ 调识别接口。
type AliyunASR struct {
	cfg Config

	mu          sync.Mutex
	token       string
	tokenExpire time.Time // Token 过期时间（秒级 unix 转 time）
}

func NewAliyunASR(cfg Config) *AliyunASR {
	return &AliyunASR{cfg: cfg}
}

// ---------------- Token：POP 协议签名换取 ----------------

// createTokenResp 是 CreateToken 接口返回的结构（只取我们要的字段）。
type createTokenResp struct {
	Token struct {
		ID        string `json:"Id"`
		ExpireTime int64  `json:"ExpireTime"` // 秒级 unix 时间戳
	} `json:"Token"`
	// 出错时阿里云返回这些
	Code      string `json:"Code"`
	Message   string `json:"Message"`
	RequestID string `json:"RequestId"`
}

// getToken 返回有效 Token：缓存未过期就复用，否则重新签名换取。
func (a *AliyunASR) getToken() (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	// 提前 60 秒判过期，留点余量
	if a.token != "" && time.Now().Before(a.tokenExpire.Add(-60*time.Second)) {
		return a.token, nil
	}

	tok, exp, err := a.requestToken()
	if err != nil {
		return "", err
	}
	a.token = tok
	a.tokenExpire = time.Unix(exp, 0)
	return tok, nil
}

// requestToken 实现阿里云 POP 协议签名，调用 CreateToken。
// 签名规范：参数按字典序排 -> 规范化查询串 -> StringToSign="GET&%2F&"+编码(查询串)
//          -> HMAC-SHA1(StringToSign, AccessKeySecret+"&") -> base64 -> 作为 Signature 参数。
func (a *AliyunASR) requestToken() (string, int64, error) {
	endpoint := fmt.Sprintf("http://nls-meta.%s.aliyuncs.com/", a.cfg.Region)

	// 1. 公共参数（不含 Signature）
	params := map[string]string{
		"AccessKeyId":      a.cfg.AccessKeyID,
		"Action":           "CreateToken",
		"Format":           "JSON",
		"RegionId":         a.cfg.Region,
		"SignatureMethod":  "HMAC-SHA1",
		"SignatureNonce":   nonce(),
		"SignatureVersion": "1.0",
		"Timestamp":        time.Now().UTC().Format("2006-01-02T15:04:05Z"),
		"Version":          "2019-02-28",
	}

	// 2. 按 key 字典序排，拼成规范化查询串（值和键都要 POP 专用编码）
	keys := make([]string, 0, len(params))
	for k := range params {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var pairs []string
	for _, k := range keys {
		pairs = append(pairs, popEncode(k)+"="+popEncode(params[k]))
	}
	canonical := strings.Join(pairs, "&")

	// 3. StringToSign = HTTPMethod + "&" + encode("/") + "&" + encode(canonical)
	stringToSign := "GET&" + popEncode("/") + "&" + popEncode(canonical)

	// 4. HMAC-SHA1，key = AccessKeySecret + "&"
	mac := hmac.New(sha1.New, []byte(a.cfg.AccessKeySecret+"&"))
	mac.Write([]byte(stringToSign))
	signature := base64.StdEncoding.EncodeToString(mac.Sum(nil))

	// 5. 把 Signature 加进参数，组成最终 GET 请求
	params["Signature"] = signature
	q := url.Values{}
	for k, v := range params {
		q.Set(k, v)
	}
	reqURL := endpoint + "?" + q.Encode()

	resp, err := http.Get(reqURL)
	if err != nil {
		return "", 0, fmt.Errorf("请求 CreateToken 失败: %w", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)

	var r createTokenResp
	if err := json.Unmarshal(body, &r); err != nil {
		return "", 0, fmt.Errorf("解析 CreateToken 响应失败: %w, body=%s", err, string(body))
	}
	if r.Token.ID == "" {
		return "", 0, fmt.Errorf("获取 Token 失败: code=%s msg=%s body=%s", r.Code, r.Message, string(body))
	}
	return r.Token.ID, r.Token.ExpireTime, nil
}

// ---------------- 识别：POST PCM，返回文字 ----------------

type asrResp struct {
	TaskID  string `json:"task_id"`
	Result  string `json:"result"`
	Status  int    `json:"status"`
	Message string `json:"message"`
}

// Recognize 把一段 PCM 音频(16k/16bit/单声道)发给阿里云，返回识别文字。
func (a *AliyunASR) Recognize(pcm []byte) (string, error) {
	token, err := a.getToken()
	if err != nil {
		return "", err
	}

	endpoint := fmt.Sprintf("https://nls-gateway-%s.aliyuncs.com/stream/v1/asr", a.cfg.Region)
	q := url.Values{}
	q.Set("appkey", a.cfg.AppKey)
	q.Set("format", "pcm")
	q.Set("sample_rate", "16000")
	q.Set("enable_punctuation_prediction", "true")
	q.Set("enable_inverse_text_normalization", "true")
	reqURL := endpoint + "?" + q.Encode()

	req, err := http.NewRequest(http.MethodPost, reqURL, strings.NewReader(string(pcm)))
	if err != nil {
		return "", err
	}
	req.Header.Set("X-NLS-Token", token)
	req.Header.Set("Content-Type", "application/octet-stream")

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", fmt.Errorf("请求识别接口失败: %w", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)

	var r asrResp
	if err := json.Unmarshal(body, &r); err != nil {
		return "", fmt.Errorf("解析识别响应失败: %w, body=%s", err, string(body))
	}
	if r.Status != 20000000 {
		return "", fmt.Errorf("识别失败: status=%d msg=%s body=%s", r.Status, r.Message, string(body))
	}
	return r.Result, nil
}

// ---------------- 工具 ----------------

// popEncode 是阿里云 POP 协议要求的 URL 编码：在标准 encode 基础上，
// 把 + 换成 %20、* 换成 %2A、%7E 换回 ~。
func popEncode(s string) string {
	e := url.QueryEscape(s)
	e = strings.ReplaceAll(e, "+", "%20")
	e = strings.ReplaceAll(e, "*", "%2A")
	e = strings.ReplaceAll(e, "%7E", "~")
	return e
}

// nonce 生成一个签名用的随机串（用纳秒时间，够唯一）。
func nonce() string {
	return fmt.Sprintf("%d", time.Now().UnixNano())
}
