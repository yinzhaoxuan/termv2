#include "io_queues.h"

namespace it2w {

std::size_t utf8CutPoint(std::string_view s, std::size_t max) {
    if (s.size() <= max) return s.size();
    std::size_t cut = max;
    // 回退到非续字节处；最多回退 3 字节，若仍找不到（非法序列）就按 max 切
    for (std::size_t k = 0; k < 4 && cut > 0; ++k) {
        if ((static_cast<unsigned char>(s[cut]) & 0xc0) != 0x80) return cut;
        --cut;
    }
    return max;
}

EnqueueResult InputScheduler::enqueueUser(std::string_view bytes) {
    if (closed_) return EnqueueResult::InvalidState;
    if (bytes.empty()) return EnqueueResult::Ok;
    if (userBytes_ + bytes.size() > kUserQueueCap) return EnqueueResult::QueueFull;
    const std::size_t total = bytes.size();
    while (!bytes.empty()) {
        std::size_t n = utf8CutPoint(bytes, kTextUnitMax);
        user_.push_back(Unit{InputKind::UserText, std::string(bytes.substr(0, n))});
        bytes.remove_prefix(n);
    }
    userBytes_ += total;
    return EnqueueResult::Ok;
}

EnqueueResult InputScheduler::enqueueReply(std::string_view bytes) {
    if (closed_) return EnqueueResult::InvalidState;
    if (bytes.size() > kReplyQueueCap) return EnqueueResult::TooLarge;
    if (replyBytes_ + bytes.size() > kReplyQueueCap) return EnqueueResult::QueueFull;  // → ReplyBackpressure
    reply_.push_back(Unit{InputKind::Reply, std::string(bytes)});
    replyBytes_ += bytes.size();
    return EnqueueResult::Ok;
}

static const char kPasteStart[] = "\x1b[200~", kPasteEnd[] = "\x1b[201~";

EnqueueResult InputScheduler::beginPaste(std::size_t total, bool bracketed) {
    if (closed_) return EnqueueResult::InvalidState;
    if (paste_) return EnqueueResult::PasteInProgress;
    if (total > kPasteMax) return EnqueueResult::TooLarge;  // 不自动拆分
    if (userBytes_ + total + 12 > kUserQueueCap) return EnqueueResult::QueueFull;
    Unit u{InputKind::PasteTransaction, bracketed ? kPasteStart : "", 0, false};
    paste_ = Paste{total, bracketed};
    if (total == 0) { if (bracketed) u.bytes += kPasteEnd; u.complete = true; paste_.reset(); }
    userBytes_ += u.bytes.size();
    user_.push_back(std::move(u));
    return EnqueueResult::Ok;
}

EnqueueResult InputScheduler::appendPaste(std::string_view body) {
    if (!paste_) return EnqueueResult::NoPaste;
    if (body.size() > paste_->remaining) return EnqueueResult::TooLarge;
    // 事务单元可能已成为当前写入单元
    Unit *u = (cur_ && !cur_->complete) ? &*cur_ : &user_.back();
    u->bytes.append(body);
    userBytes_ += body.size();
    paste_->remaining -= body.size();
    if (paste_->remaining == 0) {
        if (paste_->bracketed) { u->bytes += kPasteEnd; userBytes_ += 6; }
        u->complete = true;
        paste_.reset();
    }
    return EnqueueResult::Ok;
}

void InputScheduler::cancelPasteBody() {
    if (!paste_) return;
    Unit *u = (cur_ && !cur_->complete) ? &*cur_ : &user_.back();
    if (paste_->bracketed) { u->bytes += kPasteEnd; userBytes_ += 6; }  // 尽力补发结束标记
    u->complete = true;
    paste_.reset();
}

bool InputScheduler::pickNext() {
    if (cur_) return true;
    // 优先级：应答 > 尚未开始的普通输入；事务一旦开始（cur_）必须写完
    if (!reply_.empty()) { cur_ = std::move(reply_.front()); reply_.pop_front(); return true; }
    if (!user_.empty()) { cur_ = std::move(user_.front()); user_.pop_front(); return true; }
    return false;
}

std::string_view InputScheduler::peekWrite() {
    if (!pickNext()) return {};
    return std::string_view(cur_->bytes).substr(cur_->off);
}

void InputScheduler::consumed(std::size_t n, uint64_t nowMs) {
    if (!cur_ || n == 0) return;
    cur_->off += n;
    if (cur_->kind == InputKind::Reply) replyBytes_ -= n; else userBytes_ -= n;
    lastProgress_ = nowMs;
    pendingSince_.reset();
    if (cur_->off == cur_->bytes.size() && cur_->complete) cur_.reset();
    if (hasPending()) pendingSince_ = nowMs;
}

bool InputScheduler::hasPending() const {
    if (cur_ && cur_->off < cur_->bytes.size()) return true;
    if (cur_ && !cur_->complete) return false;  // 等待粘贴正文，不算停滞
    return !reply_.empty() || !user_.empty();
}

bool InputScheduler::stalled(uint64_t nowMs) const {
    return pendingSince_ && hasPending() && nowMs - *pendingSince_ >= kStallMs;
}

OutputQueue::PushResult OutputQueue::push(std::string chunk) {
    if (closing_) {
        if (drainExtra_ + chunk.size() > kCloseDrainExtra) { discarded_ += chunk.size(); return PushResult::Discarded; }
        drainExtra_ += chunk.size();
    } else if (bytes_ + chunk.size() > kOutputCap) {
        // 读线程在 3 MiB 停读，单次读不超过 64 KiB，正常不会到这里
        discarded_ += chunk.size();
        return PushResult::Overflow;
    }
    bytes_ += chunk.size();
    if (bytes_ > peak_) peak_ = bytes_;
    q_.push_back(std::move(chunk));
    if (!closing_ && bytes_ >= kOutputPause) { paused_ = true; return PushResult::PauseReading; }
    return PushResult::Ok;
}

std::optional<std::string> OutputQueue::pop() {
    if (q_.empty()) return std::nullopt;
    std::string s = std::move(q_.front());
    q_.pop_front();
    bytes_ -= s.size();
    return s;
}

}  // namespace it2w
