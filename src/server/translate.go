package main

import (
	"fmt"
	"unicode"
)

// hasChinese 判断字符串里是否含中文字符，用来决定翻译方向。
func hasChinese(s string) bool {
	for _, r := range s {
		if unicode.Is(unicode.Han, r) {
			return true
		}
	}
	return false
}

// Translate 自动判方向：有中文 -> 译成英文；否则 -> 译成中文。
// 复用 DeepSeek，给翻译专用提示，只返回译文。返回 (译文, 目标语言名)。
func (d *DeepSeek) Translate(text string) (string, string, error) {
	toEnglish := hasChinese(text)
	var sys, targetName string
	// 翻译【不带会话历史】，每句独立翻。
	// 之前带历史会在连续叙事(如朗读小说)里让模型搞混"当前句"，导致译文错位。
	if toEnglish {
		sys = "你是翻译引擎。把用户输入翻译成地道自然的英文。只输出英文译文本身，不要任何解释、引号、拼音。"
		targetName = "英文"
	} else {
		sys = "你是翻译引擎。把用户输入翻译成地道自然的简体中文。只输出中文译文本身，不要任何解释、引号。"
		targetName = "中文"
	}

	reply, err := d.chatWithSystem(sys, text)   // 单句，无历史
	if err != nil {
		return "", targetName, fmt.Errorf("翻译失败: %w", err)
	}
	return reply, targetName, nil
}
