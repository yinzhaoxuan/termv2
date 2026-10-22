// SPDX-License-Identifier: GPL-2.0-only
//
// iTerm2 Windows 派生核心：VT 序列解析器（T04）。
// 派生自 iTerm2 固定提交 f29420ae4d3e0f986ee24ea286885d32af58175a 的
// sources/VT100/VT100CSIParser.m（AdvanceAndEatControlChars、ParseCSIPrefix、
// ParseCSIParameters、ParseCSIIntermediate、ParseCSIGarbage、ParseCSIFinal）
// 与 sources/VT100/iTermParser.h（iTermParserContext 游标、CSIParam、参数/子参数上限）。
// Copyright (c) George Nachman and iTerm2 contributors. Licensed under GNU GPL v2 (see upstream LICENSE).
// 修改原因见 ports/core/SOURCE_MAP.md。
#include "vt_parser.h"

namespace itcore {
namespace {
constexpr uint8_t ESC = 0x1b, CAN = 0x18, SUB = 0x1a, BEL = 0x07, DEL = 0x7f;

// 上游 AdvanceAndEatControlChars 中作为 incidental token 输出的控制字符。
bool is_incidental(uint8_t c) {
    switch (c) {
        case 0x05: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c:
        case 0x0d: case 0x0e: case 0x0f: case 0x11: case 0x13: case DEL:
            return true;
        default:
            return false;
    }
}

const char kReplacement[] = "\xEF\xBF\xBD";

// 字节串 -> JSON 字符串（非法 UTF-8 替换为 U+FFFD）。
void json_string(std::string &out, const std::string &in) {
    out += '"';
    std::size_t i = 0, n = in.size();
    auto u = [&](std::size_t k) { return uint8_t(in[k]); };
    while (i < n) {
        uint8_t b = u(i);
        if (b < 0x80) {
            if (b == '"') out += "\\\"";
            else if (b == '\\') out += "\\\\";
            else if (b < 0x20 || b == DEL) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", b); out += buf; }
            else out += char(b);
            ++i;
            continue;
        }
        int need = (b >= 0xc2 && b <= 0xdf) ? 1 : (b >= 0xe0 && b <= 0xef) ? 2 : (b >= 0xf0 && b <= 0xf4) ? 3 : -1;
        bool ok = need > 0;
        if (ok) for (int k = 1; k <= need; ++k) if (i + k >= n || (u(i + k) & 0xc0) != 0x80) { ok = false; break; }
        if (ok) { out.append(in, i, need + 1); i += need + 1; }
        else { out += kReplacement; ++i; }
    }
    out += '"';
}
}  // namespace

void Parser::emit(Token &&t, TokenSink &sink) {
    flush_text(sink);
    sink.token(t);
}
void Parser::emit_simple(TokenType ty, TokenSink &sink) {
    Token t; t.type = ty; emit(std::move(t), sink);
}
void Parser::flush_text(TokenSink &sink) {
    if (text_.empty()) return;
    Token t; t.type = TokenType::Text; t.text.swap(text_);
    sink.token(t);
}

void Parser::feed(const uint8_t *data, std::size_t len, TokenSink &sink) {
    for (std::size_t i = 0; i < len && state_ != State::Dead; ++i) step(data[i], sink);
}

void Parser::finish(TokenSink &sink) {
    bool incomplete = utf8_need_ > 0 || (state_ != State::Ground && state_ != State::Dead);
    flush_text(sink);
    if (incomplete) emit_simple(TokenType::Incomplete, sink);
    utf8_need_ = utf8_have_ = 0;
    if (state_ != State::Dead) state_ = State::Ground;
}

// ---- CSI（移植自 VT100CSIParser.m）----
void Parser::csi_begin() {  // CSIParamInitialize
    state_ = State::Csi; phase_ = CsiPhase::Params; csi_first_ = true;
    unrecognized_ = trunc_ = false; priv_ = 0; inter_.clear(); params_.clear();
    cur_param_.clear(); cur_has_digit_ = cur_has_colon_ = false; cur_value_ = 0;
}
void Parser::csi_end_part() {
    std::optional<int32_t> v;
    if (cur_has_digit_) v = int32_t(cur_value_);
    if (cur_param_.size() < 1 + kMaxSubParams) cur_param_.push_back(v);
    else trunc_ = true;
    cur_has_digit_ = false; cur_value_ = 0;
}
void Parser::csi_end_param() {
    csi_end_part();
    if (params_.size() < kMaxParams) params_.push_back(std::move(cur_param_));
    else trunc_ = true;  // 上游：超出 VT100CSIPARAM_MAX 的参数被丢弃
    cur_param_.clear(); cur_has_colon_ = false;
}
void Parser::csi_finish(uint8_t final, TokenSink &sink) {  // ParseCSIFinal
    // 结束最后一个参数；与上游一致：最后一段完全为空（无数字、无冒号）时不计数。
    if (cur_has_digit_ || cur_has_colon_) csi_end_param();
    state_ = State::Ground;
    if (unrecognized_) { emit_simple(TokenType::Invalid, sink); return; }
    Token t; t.type = TokenType::Csi; t.priv = priv_; t.inter = inter_; t.final = char(final);
    t.params = std::move(params_); t.trunc = trunc_;
    emit(std::move(t), sink);
}

// ---- 字符串序列（OSC/DCS/SOS/PM/APC），设计 15.2 ----
void Parser::str_begin(TokenType ty) {
    state_ = State::Str; str_type_ = ty; payload_.clear(); discarded_ = 0;
}
void Parser::str_append(uint8_t b, TokenSink &sink) {
    if (payload_.size() < kMaxStringPayload) { payload_ += char(b); return; }
    if (++discarded_ > kMaxDiscard) {
        Token t; t.type = TokenType::Error; t.error = "ParserLimitExceeded";
        emit(std::move(t), sink);
        payload_.clear(); payload_.shrink_to_fit();
        state_ = State::Dead;
    }
}
void Parser::str_finish(TokenSink &sink) {
    Token t; t.type = str_type_; t.text.swap(payload_); t.trunc = discarded_ > 0;
    state_ = State::Ground;
    emit(std::move(t), sink);
}

void Parser::step(uint8_t b, TokenSink &sink) {
    for (;;) {  // 'continue' = 在新状态下重新处理当前字节
        switch (state_) {
        case State::Dead:
            return;
        case State::Ground:
            if (utf8_need_ > 0) {
                if ((b & 0xc0) == 0x80) {
                    utf8_buf_[utf8_have_++] = b;
                    if (utf8_have_ == utf8_need_ + 1) {
                        text_.append(reinterpret_cast<char *>(utf8_buf_), utf8_have_);
                        utf8_need_ = utf8_have_ = 0;
                    }
                    return;
                }
                text_ += kReplacement;
                utf8_need_ = utf8_have_ = 0;
            }
            if (b == ESC) { flush_text(sink); state_ = State::Esc; esc_inter_.clear(); return; }
            if (b < 0x20 || b == DEL) { Token t; t.type = TokenType::C0; t.c = b; emit(std::move(t), sink); return; }
            if (b < 0x80) { text_ += char(b); return; }
            if (b >= 0xc2 && b <= 0xdf) utf8_need_ = 1;
            else if (b >= 0xe0 && b <= 0xef) utf8_need_ = 2;
            else if (b >= 0xf0 && b <= 0xf4) utf8_need_ = 3;
            else { text_ += kReplacement; return; }
            utf8_buf_[0] = b; utf8_have_ = 1;
            return;

        case State::Esc:
        case State::EscInter:
            if (b == CAN || b == SUB) { emit_simple(TokenType::Invalid, sink); state_ = State::Ground; continue; }
            if (b == ESC) { emit_simple(TokenType::Invalid, sink); esc_inter_.clear(); state_ = State::Esc; return; }
            if (b < 0x20) { Token t; t.type = TokenType::C0; t.c = b; emit(std::move(t), sink); return; }
            if (b == DEL) return;
            if (b >= 0x80) { emit_simple(TokenType::Invalid, sink); state_ = State::Ground; continue; }
            if (b <= 0x2f) {
                if (esc_inter_.size() < kMaxIntermediates) esc_inter_ += char(b);
                state_ = State::EscInter; return;
            }
            if (state_ == State::Esc) {
                switch (b) {
                    case '[': csi_begin(); return;
                    case ']': str_begin(TokenType::Osc); return;
                    case 'P': str_begin(TokenType::Dcs); return;
                    case 'X': str_begin(TokenType::Sos); return;
                    case '^': str_begin(TokenType::Pm); return;
                    case '_': str_begin(TokenType::Apc); return;
                    default: break;
                }
            }
            { Token t; t.type = TokenType::Esc; t.inter = esc_inter_; t.final = char(b);
              state_ = State::Ground; emit(std::move(t), sink); }
            return;

        case State::Csi:
            // AdvanceAndEatControlChars：CAN/SUB/ESC 取消序列；部分 C0 作为 incidental 输出；其余 C0 吞掉。
            if (b == CAN || b == SUB || b == ESC) {
                emit_simple(TokenType::Invalid, sink); state_ = State::Ground; continue;
            }
            if (b < 0x20 || b == DEL) {
                if (is_incidental(b)) { Token t; t.type = TokenType::C0; t.c = b; emit(std::move(t), sink); }
                return;
            }
            if (phase_ == CsiPhase::Params) {
                if (csi_first_) {  // ParseCSIPrefix
                    csi_first_ = false;
                    if (b >= '<' && b <= '?') { priv_ = char(b); return; }
                }
                if (b >= 0x30 && b <= 0x3f) {  // ParseCSIParameters
                    if (b >= '0' && b <= '9') {
                        cur_has_digit_ = true;
                        cur_value_ = cur_value_ * 10 + (b - '0');
                        if (cur_value_ > kMaxParamValue) { cur_value_ = kMaxParamValue; trunc_ = true; }
                    } else if (b == ';') {
                        csi_end_param();
                    } else if (b == ':') {
                        cur_has_colon_ = true; csi_end_part();
                    } else {
                        unrecognized_ = true;  // 参数中间出现 < = > ?
                    }
                    return;
                }
                phase_ = CsiPhase::Inter;
            }
            if (phase_ == CsiPhase::Inter) {  // ParseCSIIntermediate
                if (b >= 0x20 && b <= 0x2f) {
                    if (inter_.size() < kMaxIntermediates) inter_ += char(b); else trunc_ = true;
                    return;
                }
                phase_ = CsiPhase::Garbage;
            }
            // ParseCSIGarbage / ParseCSIFinal
            if (b >= 0x40 && b <= 0x7e) { csi_finish(b, sink); return; }
            unrecognized_ = true;
            return;

        case State::Str:
            if (b == ESC) { state_ = State::StrEsc; return; }
            if (b == CAN || b == SUB) { emit_simple(TokenType::Invalid, sink); payload_.clear(); state_ = State::Ground; continue; }
            if (b == BEL && str_type_ == TokenType::Osc) { str_finish(sink); return; }
            str_append(b, sink);
            return;

        case State::StrEsc:
            if (b == '\\') { str_finish(sink); return; }
            emit_simple(TokenType::Invalid, sink); payload_.clear();
            state_ = State::Esc; esc_inter_.clear();
            continue;
        }
    }
}

std::string to_json(const Token &t) {
    std::string o;
    auto ch = [](char c) { return c ? std::string(1, c) : std::string(); };
    switch (t.type) {
    case TokenType::Text: o = "{\"t\":\"TEXT\",\"s\":"; json_string(o, t.text); o += "}"; break;
    case TokenType::C0: o = "{\"t\":\"C0\",\"c\":" + std::to_string(t.c) + "}"; break;
    case TokenType::Csi: {
        o = "{\"t\":\"CSI\",\"priv\":"; json_string(o, ch(t.priv));
        o += ",\"inter\":"; json_string(o, t.inter);
        o += ",\"final\":"; json_string(o, ch(t.final));
        o += ",\"p\":[";
        for (std::size_t i = 0; i < t.params.size(); ++i) {
            if (i) o += ',';
            o += '[';
            for (std::size_t j = 0; j < t.params[i].size(); ++j) {
                if (j) o += ',';
                o += t.params[i][j] ? std::to_string(*t.params[i][j]) : "null";
            }
            o += ']';
        }
        o += "],\"trunc\":"; o += t.trunc ? "true" : "false"; o += "}";
        break;
    }
    case TokenType::Esc:
        o = "{\"t\":\"ESC\",\"inter\":"; json_string(o, t.inter);
        o += ",\"final\":"; json_string(o, ch(t.final)); o += "}"; break;
    case TokenType::Osc: case TokenType::Dcs: case TokenType::Sos: case TokenType::Pm: case TokenType::Apc: {
        const char *n = t.type == TokenType::Osc ? "OSC" : t.type == TokenType::Dcs ? "DCS"
                      : t.type == TokenType::Sos ? "SOS" : t.type == TokenType::Pm ? "PM" : "APC";
        o = std::string("{\"t\":\"") + n + "\",\"s\":"; json_string(o, t.text);
        o += ",\"trunc\":"; o += t.trunc ? "true" : "false"; o += "}"; break;
    }
    case TokenType::Invalid: o = "{\"t\":\"INVALID\"}"; break;
    case TokenType::Incomplete: o = "{\"t\":\"INCOMPLETE\"}"; break;
    case TokenType::Error: o = "{\"t\":\"ERROR\",\"e\":"; json_string(o, t.error); o += "}"; break;
    }
    return o;
}

}  // namespace itcore
