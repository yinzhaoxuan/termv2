// 输入调度与输出水位（T03A 规格 §2.4、§2.5，设计 15.2）。纯逻辑、非线程安全：
// ConPTY 传输层在自己的锁内调用；Linux 上单测。时间由调用方传入（毫秒，单调时钟）。
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace it2w {

inline constexpr std::size_t kUserQueueCap = 4u << 20;
inline constexpr std::size_t kReplyQueueCap = 64u << 10;
inline constexpr std::size_t kTextUnitMax = 4u << 10;
inline constexpr std::size_t kPasteMax = 32u << 10;
inline constexpr uint64_t kStallMs = 5000;
inline constexpr std::size_t kOutputCap = 4u << 20, kOutputPause = 3u << 20, kOutputResume = 1u << 20;
inline constexpr std::size_t kCloseDrainExtra = 4u << 20;

enum class InputKind { UserText, PasteTransaction, Reply };
enum class EnqueueResult { Ok, QueueFull, TooLarge, InvalidState, PasteInProgress, NoPaste };

// 在不超过 max 的前提下找到 UTF-8 边界（不切开多字节字符）。
std::size_t utf8CutPoint(std::string_view s, std::size_t max);

class InputScheduler {
public:
    // 用户文本：按 4 KiB 与 UTF-8 边界切成多个单元；整体放不下则整体拒绝。
    EnqueueResult enqueueUser(std::string_view bytes);
    // 协议应答：一个单元，单个应答不得超过队列容量。
    EnqueueResult enqueueReply(std::string_view bytes);
    // bracketed paste 事务：begin 写入开始标记并预留 totalBytes，append 追加正文，
    // 正文达到 totalBytes 时自动补结束标记；cancelPasteBody 丢弃未写入正文并立即补结束标记。
    EnqueueResult beginPaste(std::size_t totalBytes, bool bracketed = true);
    EnqueueResult appendPaste(std::string_view body);
    void cancelPasteBody();
    // 关闭：拒绝新单元。
    void close() { closed_ = true; }

    // 写线程：取当前应写的字节（当前单元剩余部分）；空表示无可写（或事务正文尚未到达）。
    std::string_view peekWrite();
    InputKind currentKind() const { return cur_ ? cur_->kind : InputKind::UserText; }
    void consumed(std::size_t n, uint64_t nowMs);
    // 有待写数据且 kStallMs 内无进展 → true（InputStalled）。
    bool stalled(uint64_t nowMs) const;
    void notePending(uint64_t nowMs) { if (!pendingSince_) pendingSince_ = nowMs; }
    bool hasPending() const;

    std::size_t userBytes() const { return userBytes_; }
    std::size_t replyBytes() const { return replyBytes_; }
    bool pasteOpen() const { return paste_.has_value(); }

private:
    struct Unit { InputKind kind{}; std::string bytes; std::size_t off = 0; bool complete = true; };
    struct Paste { std::size_t remaining; bool bracketed; };
    bool pickNext();
    std::deque<Unit> user_, reply_;
    std::optional<Unit> cur_;
    std::optional<Paste> paste_;  // 事务进行中：正文仍在追加
    std::size_t userBytes_ = 0, replyBytes_ = 0;
    std::optional<uint64_t> pendingSince_;
    uint64_t lastProgress_ = 0;
    bool closed_ = false;
};

// 输出队列：Running 时 4 MiB 上限、3 MiB 停读、1 MiB 恢复；Closing 走排空路径，
// 额外 4 MiB 后有界丢弃并标记数据不完整。
class OutputQueue {
public:
    enum class PushResult { Ok, PauseReading, Discarded, Overflow };
    PushResult push(std::string chunk);
    std::optional<std::string> pop();
    bool readingPaused() const { return paused_; }
    // pop 后检查：从暂停回落到 1 MiB 以下时返回 true（应唤醒读线程）
    bool shouldResume() const { return paused_ && bytes_ <= kOutputResume; }
    void resumed() { paused_ = false; }
    void beginClosing() { closing_ = true; paused_ = false; }
    bool incomplete() const { return discarded_ > 0; }
    uint64_t discardedBytes() const { return discarded_; }
    std::size_t bytes() const { return bytes_; }
    std::size_t peak() const { return peak_; }

private:
    std::deque<std::string> q_;
    std::size_t bytes_ = 0, peak_ = 0, drainExtra_ = 0;
    uint64_t discarded_ = 0;
    bool paused_ = false, closing_ = false;
};

}  // namespace it2w
