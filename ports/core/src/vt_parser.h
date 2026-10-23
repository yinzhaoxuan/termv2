// SPDX-License-Identifier: GPL-2.0-only
//
// iTerm2 Windows 派生核心：VT 序列解析器（T04）。
// 派生自 iTerm2（https://github.com/gnachman/iTerm2，固定提交
// f29420ae4d3e0f986ee24ea286885d32af58175a）的 sources/VT100/VT100CSIParser.m
// 与 sources/VT100/iTermParser.h，原作者 George Nachman 及 iTerm2 贡献者。
// iTerm2 以 GNU GPL v2 发布（见上游 LICENSE）；本文件同样按 GPL v2 发布。
// 修改：去除 Foundation/AppKit 依赖，改写为 C++20 有状态逐字节状态机，
// 支持跨分块解析；详细对照与修改原因见 ports/core/SOURCE_MAP.md。
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace itcore {

// 上游 VT100CSIPARAM_MAX / VT100CSISUBPARAM_MAX 均为 16。
inline constexpr std::size_t kMaxParams = 16;
inline constexpr std::size_t kMaxSubParams = 16;      // 每个参数的子参数上限
inline constexpr std::size_t kMaxIntermediates = 4;
inline constexpr int32_t kMaxParamValue = INT32_MAX;  // 数值饱和上限
inline constexpr std::size_t kMaxStringPayload = 1u << 20;  // OSC/DCS 1 MiB（设计 15.2）
inline constexpr std::size_t kMaxDiscard = 1u << 20;        // 超限后丢弃预算

enum class TokenType { Text, C0, Csi, Esc, Osc, Dcs, Sos, Pm, Apc, Invalid, Incomplete, Error };

struct Token {
    TokenType type{};
    std::string text;      // Text: UTF-8；Osc/Dcs...: 原始负载
    uint8_t c = 0;         // C0
    char priv = 0;         // CSI 前缀 < = > ?
    std::string inter;     // 中间字节
    char final = 0;
    // 每个参数：[值, 子参数...]；std::nullopt 表示省略
    std::vector<std::vector<std::optional<int32_t>>> params;
    bool trunc = false;    // 参数个数/数值/负载被截断或饱和
    bool bel_term = false; // OSC 以 BEL 结尾（应答沿用相同结尾符）；不进 JSON
    std::string error;     // Error
};

class TokenSink {
public:
    virtual ~TokenSink() = default;
    virtual void token(const Token &t) = 0;
};

class Parser {
public:
    // 喂入任意切分的字节；结果与切分方式无关。
    void feed(const uint8_t *data, std::size_t len, TokenSink &sink);
    // 立即输出已累积的文本（流式使用时调用；测试驱动在 finish 时统一合并）。
    void flush_text(TokenSink &sink);
    // EOF：丢弃未完成序列并输出 INCOMPLETE。
    void finish(TokenSink &sink);
    bool dead() const { return state_ == State::Dead; }
    bool utf8_pending() const { return utf8_need_ > 0; }

private:
    enum class State { Ground, Esc, EscInter, Csi, Str, StrEsc, Dead };
    enum class CsiPhase { Params, Inter, Garbage };
    void step(uint8_t b, TokenSink &sink);
    void emit(Token &&t, TokenSink &sink);
    void emit_simple(TokenType ty, TokenSink &sink);
    void csi_begin();
    void csi_end_part();
    void csi_end_param();
    void csi_finish(uint8_t final, TokenSink &sink);
    void str_begin(TokenType ty);
    void str_append(uint8_t b, TokenSink &sink);
    void str_finish(TokenSink &sink, bool bel = false);

    State state_ = State::Ground;
    std::string text_;
    uint8_t utf8_buf_[4]{};
    int utf8_have_ = 0, utf8_need_ = 0;
    // ESC
    std::string esc_inter_;
    // CSI
    CsiPhase phase_ = CsiPhase::Params;
    bool csi_first_ = true, unrecognized_ = false, trunc_ = false;
    char priv_ = 0;
    std::string inter_;
    std::vector<std::vector<std::optional<int32_t>>> params_;
    std::vector<std::optional<int32_t>> cur_param_;
    bool cur_has_digit_ = false, cur_has_colon_ = false;
    int64_t cur_value_ = 0;
    // 字符串序列
    TokenType str_type_ = TokenType::Osc;
    std::string payload_;
    std::size_t discarded_ = 0;
};

std::string to_json(const Token &t);

}  // namespace itcore
