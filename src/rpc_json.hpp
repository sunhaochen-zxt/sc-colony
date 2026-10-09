// 星际争霸：殖民地 (Star Colony) —— C++20 命令行殖民地经营游戏
// Copyright (C) 2026 Star Colony contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// See the LICENSE file at the project root for the full license text.
// SPDX-License-Identifier: AGPL-3.0-or-later

// 星际争霸：殖民地 —— 极简 JSON 读写（P2 RPC 协议专用）
//
// 设计取舍：
//   * 项目至今「零外部依赖」，因此不引入任何 JSON 第三方库；本文件只依赖 <string>/<vector>。
//   * 本项目不追求完整 JSON 规范（如不做 \u 之外的花哨特性），只覆盖 docs/PROTOCOL.md 用到的形态：
//       - 写出：字符串（正确转义 " \ 及控制字符；中文等 UTF-8 原样字节输出，不转成 \uXXXX）、
//               整数、浮点、布尔、null、数组、对象嵌套；
//       - 读入：一行 JSON 值（协议里是对象），取 method(string) / id(int 或 string) / params(object)。
//   * 读入对非法输入一律返回解析错误（不崩溃）。
//
// 命名空间：sc::json，避免与引擎命名空间 sc 里的符号混淆。
#pragma once

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sc {
namespace json {

// ============================================================
//  UTF-8 辅助（仅供 \uXXXX 解码用；中文原样字节直接透传）
// ============================================================
inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// ============================================================
//  Json 值
// ============================================================
class Json {
public:
    enum class Type { Null, Bool, Int, Real, Str, Arr, Obj };

    Json() = default;
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : type_(Type::Int), i_(v) {}
    Json(long long v) : type_(Type::Int), i_(v) {}
    Json(double v) : type_(Type::Real), d_(v) {}
    Json(const char* s) : type_(Type::Str), s_(s ? s : "") {}
    Json(std::string s) : type_(Type::Str), s_(std::move(s)) {}

    static Json object() { Json j; j.type_ = Type::Obj; return j; }
    static Json array() { Json j; j.type_ = Type::Arr; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isInt() const { return type_ == Type::Int; }
    bool isReal() const { return type_ == Type::Real; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Real; }
    bool isString() const { return type_ == Type::Str; }
    bool isArray() const { return type_ == Type::Arr; }
    bool isObject() const { return type_ == Type::Obj; }

    bool               asBool() const { return b_; }
    long long          asInt() const { return type_ == Type::Real ? static_cast<long long>(d_) : i_; }
    double             asReal() const { return type_ == Type::Int ? static_cast<double>(i_) : d_; }
    const std::string& asString() const { return s_; }

    // ---- 对象写入 ----
    // 同名键覆盖，否则追加；若当前不是对象则先转为对象。
    Json& set(const std::string& key, Json v) {
        if (type_ != Type::Obj) { type_ = Type::Obj; obj_.clear(); }
        for (auto& kv : obj_) {
            if (kv.first == key) { kv.second = std::move(v); return *this; }
        }
        obj_.emplace_back(key, std::move(v));
        return *this;
    }

    // ---- 对象读取 ----
    const Json* find(const std::string& key) const {
        if (type_ != Type::Obj) return nullptr;
        for (const auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    // ---- 数组写入 ----
    Json& push(Json v) {
        if (type_ != Type::Arr) { type_ = Type::Arr; arr_.clear(); }
        arr_.push_back(std::move(v));
        return *this;
    }
    std::size_t size() const {
        if (type_ == Type::Arr) return arr_.size();
        if (type_ == Type::Obj) return obj_.size();
        return 0;
    }

    // ---- 序列化 ----
    std::string dump() const {
        std::string out;
        out.reserve(256);
        dumpTo(out);
        return out;
    }

private:
    static void dumpStringTo(std::string& out, const std::string& s) {
        out.push_back('"');
        for (char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {                       // 其余控制字符转 \u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);                 // >=0x20 原样输出（含 UTF-8 多字节，中文不转义）
                }
                break;
            }
        }
        out.push_back('"');
    }

    static std::string formatReal(double v) {
        char buf[64];
        if (!std::isfinite(v)) return "0";             // 非有限值不该出现在协议里，退化为 0
        if (v == std::floor(v) && std::fabs(v) < 1e15) {
            std::snprintf(buf, sizeof buf, "%.1f", v); // 1.0 -> "1.0"，贴合契约示例
        } else {
            std::snprintf(buf, sizeof buf, "%.10g", v);
        }
        return buf;
    }

    void dumpTo(std::string& out) const {
        switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += (b_ ? "true" : "false"); break;
        case Type::Int:  out += std::to_string(i_); break;
        case Type::Real: out += formatReal(d_); break;
        case Type::Str:  dumpStringTo(out, s_); break;
        case Type::Arr:
            out.push_back('[');
            for (std::size_t i = 0; i < arr_.size(); ++i) {
                if (i) out.push_back(',');
                arr_[i].dumpTo(out);
            }
            out.push_back(']');
            break;
        case Type::Obj:
            out.push_back('{');
            for (std::size_t i = 0; i < obj_.size(); ++i) {
                if (i) out.push_back(',');
                dumpStringTo(out, obj_[i].first);
                out.push_back(':');
                obj_[i].second.dumpTo(out);
            }
            out.push_back('}');
            break;
        }
    }

    Type        type_ = Type::Null;
    bool        b_ = false;
    long long   i_ = 0;
    double      d_ = 0.0;
    std::string s_;
    std::vector<Json>                         arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

// ============================================================
//  解析器（递归下降，仅覆盖协议所需形态）
// ============================================================
namespace detail {

class Parser {
public:
    Parser(const std::string& s) : s_(s) {}

    bool parse(Json& out, std::string& err) {
        skipWs();
        if (!parseValue(out, err, 0)) return false;
        skipWs();
        if (pos_ != s_.size()) { err = "末尾存在多余内容"; return false; }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    const std::string& s_;
    std::size_t        pos_ = 0;

    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    void skipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool parseValue(Json& out, std::string& err, int depth) {
        if (depth > kMaxDepth) { err = "嵌套过深"; return false; }
        skipWs();
        if (eof()) { err = "意外的输入结束"; return false; }
        const char c = peek();
        switch (c) {
        case '{': return parseObject(out, err, depth);
        case '[': return parseArray(out, err, depth);
        case '"': {
            std::string sv;
            if (!parseString(sv, err)) return false;
            out = Json(std::move(sv));
            return true;
        }
        case 't': return parseLiteral("true", Json(true), out, err);
        case 'f': return parseLiteral("false", Json(false), out, err);
        case 'n': return parseLiteral("null", Json(), out, err);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out, err);
            err = std::string("非法字符 '") + c + "'";
            return false;
        }
    }

    bool parseLiteral(const char* lit, Json v, Json& out, std::string& err) {
        const std::size_t n = std::char_traits<char>::length(lit);
        if (s_.compare(pos_, n, lit) != 0) { err = "非法字面量"; return false; }
        pos_ += n;
        out = std::move(v);
        return true;
    }

    bool parseNumber(Json& out, std::string& err) {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (!(pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')) { err = "非法数字"; return false; }
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        bool integral = true;
        if (peek() == '.') {
            integral = false;
            ++pos_;
            if (!(pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')) { err = "非法数字（小数点后缺数字）"; return false; }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')) { err = "非法数字（指数缺数字）"; return false; }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string tok = s_.substr(start, pos_ - start);
        if (integral) {
            errno = 0;
            const long long v = std::strtoll(tok.c_str(), nullptr, 10);
            out = Json(v);
        } else {
            out = Json(std::strtod(tok.c_str(), nullptr));
        }
        return true;
    }

    bool parseString(std::string& out, std::string& err) {
        if (peek() != '"') { err = "期望字符串"; return false; }
        ++pos_;
        out.clear();
        while (true) {
            if (eof()) { err = "字符串未闭合"; return false; }
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) { err = "转义序列不完整"; return false; }
                const char esc = s_[pos_++];
                switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!readHex4(cp, err)) return false;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {          // 高代理项：尝试配对低位
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t lo = 0;
                            if (!readHex4(lo, err)) return false;
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            } else {
                                appendUtf8(out, 0xFFFDu);          // 落单代理项 -> 替换符
                                cp = lo;
                            }
                        } else {
                            appendUtf8(out, 0xFFFDu);
                            continue;
                        }
                    }
                    if (cp >= 0xDC00u && cp <= 0xDFFFu) appendUtf8(out, 0xFFFDu);   // 孤立低代理项
                    else appendUtf8(out, cp);
                    break;
                }
                default: err = "未知转义序列"; return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20u) {
                err = "字符串中出现未转义控制字符";
                return false;
            } else {
                out.push_back(c);   // 含 UTF-8 续字节，原样透传
            }
        }
    }

    bool readHex4(uint32_t& cp, std::string& err) {
        if (pos_ + 4 > s_.size()) { err = "\\u 转义不完整"; return false; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
            else { err = "\\u 转义含非十六进制字符"; return false; }
        }
        cp = v;
        return true;
    }

    bool parseArray(Json& out, std::string& err, int depth) {
        ++pos_;   // '['
        out = Json::array();
        skipWs();
        if (peek() == ']') { ++pos_; return true; }
        while (true) {
            Json item;
            if (!parseValue(item, err, depth + 1)) return false;
            out.push(std::move(item));
            skipWs();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == ']') { ++pos_; return true; }
            err = "数组缺少 ',' 或 ']'";
            return false;
        }
    }

    bool parseObject(Json& out, std::string& err, int depth) {
        ++pos_;   // '{'
        out = Json::object();
        skipWs();
        if (peek() == '}') { ++pos_; return true; }
        while (true) {
            skipWs();
            if (peek() != '"') { err = "对象键必须是字符串"; return false; }
            std::string key;
            if (!parseString(key, err)) return false;
            skipWs();
            if (peek() != ':') { err = "对象缺少 ':'"; return false; }
            ++pos_;
            Json val;
            if (!parseValue(val, err, depth + 1)) return false;
            out.set(key, std::move(val));
            skipWs();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; return true; }
            err = "对象缺少 ',' 或 '}'";
            return false;
        }
    }
};

} // namespace detail

// 解析一行 JSON 文本。成功返回 true；失败返回 false 并填充 err。
inline bool parse(const std::string& text, Json& out, std::string& err) {
    detail::Parser p(text);
    err.clear();
    return p.parse(out, err);
}

} // namespace json
} // namespace sc
