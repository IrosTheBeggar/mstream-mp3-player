// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// A small JSON reader for the host tests that read the card contract's
// shared fixtures (test/fixtures/card: vectors.json, the library
// descriptions, hardening/index.json). RFC 8259 values; numbers keep their
// text, so integers read exactly (u64() also takes the fixtures' "0x..."
// strings, used for values past 2^53); \uXXXX escapes and surrogate pairs
// become UTF-8. Host only: std::string and std::vector.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace minijson {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  std::string text;  // a String's bytes, or a Number's text
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> members;

  bool isNull() const { return type == Null; }
  bool has(const char* key) const {
    for (const auto& m : members)
      if (m.first == key) return true;
    return false;
  }
  const Value& operator[](const char* key) const {
    for (const auto& m : members)
      if (m.first == key) return m.second;
    return none();
  }
  const Value& operator[](size_t i) const { return i < items.size() ? items[i] : none(); }
  const Value& operator[](int i) const { return i >= 0 ? (*this)[static_cast<size_t>(i)] : none(); }
  size_t size() const { return type == Array ? items.size() : members.size(); }
  const std::string& str() const { return text; }
  const char* c_str() const { return text.c_str(); }
  bool boolean() const { return type == Bool && b; }
  double num() const { return type == Number ? std::strtod(text.c_str(), nullptr) : 0.0; }
  // An integer: a Number's text, or a string ("0x..." hex, or decimal).
  uint64_t u64(uint64_t fallback = 0) const {
    if (type == Number || type == String) {
      if (text.empty()) return fallback;
      if (text[0] == '-') return static_cast<uint64_t>(std::strtoll(text.c_str(), nullptr, 10));
      return std::strtoull(text.c_str(), nullptr, 0);
    }
    return fallback;
  }
  int64_t i64(int64_t fallback = 0) const {
    if (type == Number || type == String) return std::strtoll(text.c_str(), nullptr, 0);
    return fallback;
  }
  static const Value& none() {
    static const Value v;
    return v;
  }
};

class Parser {
public:
  explicit Parser(const std::string& s) : s_(s) {}
  bool parse(Value* out) {
    ws();
    if (!value(out)) return false;
    ws();
    return i_ == s_.size();
  }

private:
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool lit(const char* w) {
    size_t k = 0;
    while (w[k]) {
      if (i_ + k >= s_.size() || s_[i_ + k] != w[k]) return false;
      ++k;
    }
    i_ += k;
    return true;
  }
  static void putUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
      o += static_cast<char>(cp);
    } else if (cp < 0x800) {
      o += static_cast<char>(0xC0 | cp >> 6);
      o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      o += static_cast<char>(0xE0 | cp >> 12);
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      o += static_cast<char>(0xF0 | cp >> 18);
      o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t* v) {
    if (i_ + 4 > s_.size()) return false;
    *v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_++];
      *v <<= 4;
      if (c >= '0' && c <= '9')
        *v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        *v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        *v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }
  bool string(std::string* o) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    o->clear();
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        *o += c;
        continue;
      }
      if (i_ >= s_.size()) return false;
      const char e = s_[i_++];
      switch (e) {
        case '"': *o += '"'; break;
        case '\\': *o += '\\'; break;
        case '/': *o += '/'; break;
        case 'b': *o += '\b'; break;
        case 'f': *o += '\f'; break;
        case 'n': *o += '\n'; break;
        case 'r': *o += '\r'; break;
        case 't': *o += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
            i_ += 2;
            uint32_t lo;
            if (!hex4(&lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          putUtf8(*o, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(Value* v) {
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '{') {
      v->type = Value::Object;
      ++i_;
      ws();
      if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
      for (;;) {
        ws();
        std::string key;
        if (!string(&key)) return false;
        ws();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        ws();
        v->members.emplace_back(key, Value());
        if (!value(&v->members.back().second)) return false;
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
        return false;
      }
    }
    if (c == '[') {
      v->type = Value::Array;
      ++i_;
      ws();
      if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
      for (;;) {
        ws();
        v->items.emplace_back();
        if (!value(&v->items.back())) return false;
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
        return false;
      }
    }
    if (c == '"') {
      v->type = Value::String;
      return string(&v->text);
    }
    if (lit("true")) {
      v->type = Value::Bool;
      v->b = true;
      return true;
    }
    if (lit("false")) {
      v->type = Value::Bool;
      return true;
    }
    if (lit("null")) return true;
    const size_t start = i_;
    while (i_ < s_.size() && (std::strchr("+-0123456789.eE", s_[i_]) != nullptr)) ++i_;
    if (i_ == start) return false;
    v->type = Value::Number;
    v->text = s_.substr(start, i_ - start);
    return true;
  }

  const std::string& s_;
  size_t i_ = 0;
};

inline bool parse(const std::string& text, Value* out) {
  Parser p(text);
  *out = Value();
  return p.parse(out);
}

// A whole file's bytes; false when it can't be read.
inline bool readFile(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  std::fclose(f);
  return true;
}

}  // namespace minijson
