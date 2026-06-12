package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"sync/atomic"
)

// 把 16k/16bit/单声道 的裸 PCM 存成 WAV 文件，方便在电脑上直接播放检查音质。
// 文件存到 src/server/recordings/ 下，递增编号，不删除。

var recCounter int64

const (
	wavSampleRate = 16000
	wavBits       = 16
	wavChannels   = 1
)

// saveWAV 把 pcm 存成 recordings/rec_N.wav，返回文件路径。
func saveWAV(pcm []byte) (string, error) {
	dir := "recordings"
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	n := atomic.AddInt64(&recCounter, 1)
	path := filepath.Join(dir, fmt.Sprintf("rec_%d.wav", n))

	f, err := os.Create(path)
	if err != nil {
		return "", err
	}
	defer f.Close()

	dataLen := len(pcm)
	byteRate := wavSampleRate * wavChannels * wavBits / 8
	blockAlign := wavChannels * wavBits / 8

	// --- 标准 44 字节 WAV 头 ---
	f.WriteString("RIFF")
	binary.Write(f, binary.LittleEndian, uint32(36+dataLen)) // ChunkSize
	f.WriteString("WAVE")
	f.WriteString("fmt ")
	binary.Write(f, binary.LittleEndian, uint32(16))           // Subchunk1Size (PCM)
	binary.Write(f, binary.LittleEndian, uint16(1))            // AudioFormat = PCM
	binary.Write(f, binary.LittleEndian, uint16(wavChannels))  // 声道
	binary.Write(f, binary.LittleEndian, uint32(wavSampleRate))// 采样率
	binary.Write(f, binary.LittleEndian, uint32(byteRate))     // 字节率
	binary.Write(f, binary.LittleEndian, uint16(blockAlign))   // 块对齐
	binary.Write(f, binary.LittleEndian, uint16(wavBits))      // 位深
	f.WriteString("data")
	binary.Write(f, binary.LittleEndian, uint32(dataLen))      // 数据长度
	f.Write(pcm)                                               // PCM 数据

	return path, nil
}
