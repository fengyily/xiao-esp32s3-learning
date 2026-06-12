package main

import (
	"sync"
	"time"
)

// Session 维护一段对话的历史，让 DeepSeek 带上下文。
// 单板自用：全局一个会话即可，进程内存保存（重启清空，无需持久化）。
type Session struct {
	mu       sync.Mutex
	history  []chatMessage // 不含 system，只存 user/assistant 轮次
	lastSeen time.Time
}

const (
	maxTurns      = 10              // 最多保留最近 10 轮(user+assistant 各算)，超了丢最旧
	sessionExpire = 3 * time.Minute // 超过 3 分钟没说话 → 视为新话题，清空历史
)

var globalSession = &Session{}

// appendUserAndReply 在一次交互后调用：把本轮 user 文本和 assistant 回答存进历史。
// 调用前先 maybeReset 处理超时。返回当前应发给 LLM 的完整 messages（含 system）。
//
// 用法：
//   msgs := globalSession.buildMessages(systemPrompt, userText)  // 取历史+本句
//   reply := callLLM(msgs)
//   globalSession.commit(userText, reply)                        // 成功后存回

// buildMessages 组装发给 LLM 的消息：system + 历史 + 当前 user。
// 会先按超时判断是否清空历史。
func (s *Session) buildMessages(system, userText string) []chatMessage {
	s.mu.Lock()
	defer s.mu.Unlock()

	now := time.Now()
	if !s.lastSeen.IsZero() && now.Sub(s.lastSeen) > sessionExpire {
		s.history = nil // 超时，新话题
	}
	s.lastSeen = now

	msgs := make([]chatMessage, 0, len(s.history)+2)
	msgs = append(msgs, chatMessage{Role: "system", Content: system})
	msgs = append(msgs, s.history...)
	msgs = append(msgs, chatMessage{Role: "user", Content: userText})
	return msgs
}

// commit 把本轮 user 和 assistant 存进历史，并裁剪到最多 maxTurns*2 条。
func (s *Session) commit(userText, reply string) {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.history = append(s.history,
		chatMessage{Role: "user", Content: userText},
		chatMessage{Role: "assistant", Content: reply},
	)
	// 每轮 2 条，超过 maxTurns 轮就丢最旧的
	if max := maxTurns * 2; len(s.history) > max {
		s.history = s.history[len(s.history)-max:]
	}
}

// reset 手动清空（暂留接口，未来可接"重新开始"关键词）。
func (s *Session) reset() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.history = nil
}
