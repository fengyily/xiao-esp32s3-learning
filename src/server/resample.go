package main

import (
	"encoding/binary"
	"log"
	"net/http"
	"strconv"
)

// resampleFromRequest 读取请求 query 里的 src_rate（板子实际采样率），
// 若与目标 16000 不同则重采样到 16000。没传或等于 16000 则原样返回。
func resampleFromRequest(r *http.Request, pcm []byte) []byte {
	const target = 16000
	srcRate, _ := strconv.Atoi(r.URL.Query().Get("src_rate"))
	if srcRate <= 0 || srcRate == target {
		return pcm
	}
	out := resamplePCM(pcm, srcRate, target)
	log.Printf("重采样 %dHz -> %dHz: %d 字节 -> %d 字节", srcRate, target, len(pcm), len(out))
	return out
}

// resamplePCM 把 16bit 单声道 PCM 从 srcRate 重采样到 dstRate（线性插值）。
// 板子 PDM 实际产出约 17469Hz，阿里云只收 8000/16000，故在服务端转成 16000。
// 线性插值对语音识别足够（不是音乐，不追求高保真）。
func resamplePCM(in []byte, srcRate, dstRate int) []byte {
	if srcRate == dstRate || srcRate <= 0 || dstRate <= 0 {
		return in
	}
	// 解析成 int16 采样
	n := len(in) / 2
	src := make([]int16, n)
	for i := 0; i < n; i++ {
		src[i] = int16(binary.LittleEndian.Uint16(in[i*2:]))
	}

	// 目标采样数
	outN := int(int64(n) * int64(dstRate) / int64(srcRate))
	out := make([]byte, outN*2)

	ratio := float64(srcRate) / float64(dstRate)
	for i := 0; i < outN; i++ {
		pos := float64(i) * ratio
		idx := int(pos)
		frac := pos - float64(idx)

		var s float64
		if idx+1 < n {
			// 线性插值：相邻两个采样之间按小数位加权
			s = float64(src[idx])*(1-frac) + float64(src[idx+1])*frac
		} else {
			s = float64(src[n-1])
		}
		binary.LittleEndian.PutUint16(out[i*2:], uint16(int16(s)))
	}
	return out
}
