// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "UiStrings.h"

#include <cstdio>
#include <cstring>

namespace uistr {

namespace {

Lang g_lang = Lang::En;

// ---- what each language's tag means here (CLDR 46) ----

// The cardinal plural rules of the ten languages, for whole numbers (v = 0,
// e = 0: CLDR's operands of an integer).
enum class Rule : uint8_t {
  OneOther,  // en, de: one i = 1 and v = 0
  Spanish,   // es: one n = 1; many e = 0 and i != 0 and i % 1000000 = 0 and v = 0
  French,    // fr, pt: one i = 0,1 (pt: i = 0..1); many as es
  Italian,   // it (ca, pt-PT): one i = 1 and v = 0; many as es
  Polish,    // pl: one i = 1; few i % 10 = 2..4 and i % 100 != 12..14; many the rest
  Russian,   // ru (uk): one i % 10 = 1 and i % 100 != 11; few as pl; many the rest
  OtherOnly  // ja, zh
};

struct Locale {
  const char* tag;
  Rule rule;
  const char* group;    // the thousands mark
  const char* decimal;  // the decimal mark
  uint8_t minGroup;     // CLDR's minimumGroupingDigits: 2 leaves "1234" alone
};

// "\xC2\xA0" is the no-break space U+00A0. French's CLDR mark is the narrow
// one, U+202F, which the fonts don't have: its look-alike would be a plain
// space, where a line may break (TextFit), so the wide one stands in.
constexpr Locale kLocales[] = {
    {"en", Rule::OneOther, ",", ".", 1},
    {"de", Rule::OneOther, ".", ",", 1},
    {"es", Rule::Spanish, ".", ",", 2},
    {"fr", Rule::French, "\xC2\xA0", ",", 1},
    {"it", Rule::Italian, ".", ",", 2},
    {"ja", Rule::OtherOnly, ",", ".", 1},
    {"pl", Rule::Polish, "\xC2\xA0", ",", 2},
    {"pt", Rule::French, ".", ",", 1},  // Brazilian, as the mobile app (CLDR's "pt")
    {"ru", Rule::Russian, "\xC2\xA0", ",", 1},
    {"zh", Rule::OtherOnly, ",", ".", 1},
    {"en-XA", Rule::OneOther, ",", ".", 1},  // the pseudo-locale: English's
};

const Locale& localeOf(const char* tag) {
  if (tag) {
    for (const Locale& l : kLocales) {
      if (std::strcmp(l.tag, tag) == 0) return l;
    }
  }
  return kLocales[0];
}

constexpr uint8_t bit(Plural p) { return static_cast<uint8_t>(1u << static_cast<unsigned>(p)); }

uint8_t setOf(Rule r) {
  switch (r) {
    case Rule::OneOther: return bit(Plural::One) | bit(Plural::Other);
    case Rule::Spanish:
    case Rule::French:
    case Rule::Italian: return bit(Plural::One) | bit(Plural::Many) | bit(Plural::Other);
    case Rule::Polish:
    case Rule::Russian: return bit(Plural::One) | bit(Plural::Few) | bit(Plural::Many) | bit(Plural::Other);
    case Rule::OtherOnly: break;
  }
  return bit(Plural::Other);
}

Plural categoryOf(Rule r, uint64_t n) {
  const uint64_t m10 = n % 10, m100 = n % 100;
  const bool million = n != 0 && n % 1000000 == 0;
  switch (r) {
    case Rule::OneOther: return n == 1 ? Plural::One : Plural::Other;
    case Rule::Spanish:
    case Rule::Italian: return n == 1 ? Plural::One : million ? Plural::Many : Plural::Other;
    case Rule::French: return n <= 1 ? Plural::One : million ? Plural::Many : Plural::Other;
    case Rule::Polish:
      if (n == 1) return Plural::One;
      if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return Plural::Few;
      return Plural::Many;
    case Rule::Russian:
      if (m10 == 1 && m100 != 11) return Plural::One;
      if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return Plural::Few;
      return Plural::Many;
    case Rule::OtherOnly: break;
  }
  return Plural::Other;
}

const detail::Table& table(Lang l) {
  const unsigned i = static_cast<unsigned>(l);
  return detail::kTables[i < kLangs ? i : 0];
}

// The message's first byte in the UI's language, else English's; `t` is
// the table it came from.
const char* find(S id, const detail::Table** t) {
  const unsigned i = static_cast<unsigned>(id);
  if (i >= kStrings) {
    *t = &detail::kTables[0];
    return "";
  }
  const detail::Table& own = table(g_lang);
  if (own.off[i] != detail::kMissing) {
    *t = &own;
    return own.blob + own.off[i];
  }
  *t = &detail::kTables[0];
  return detail::kTables[0].blob + detail::kTables[0].off[i];
}

// The form for category `want` of a plural message at `p` in a language
// with categories `set`: the "other" form where that one is empty, else
// the first that isn't.
const char* form(const char* p, uint8_t set, Plural want) {
  const char* forms[6] = {};
  int count = 0, wantAt = -1, otherAt = -1;
  for (int c = 0; c < 6; ++c) {
    if (!(set & (1u << c))) continue;
    if (c == static_cast<int>(want)) wantAt = count;
    if (c == static_cast<int>(Plural::Other)) otherAt = count;
    forms[count++] = p;
    p += std::strlen(p) + 1;
  }
  if (wantAt >= 0 && forms[wantAt][0]) return forms[wantAt];
  if (otherAt >= 0 && forms[otherAt][0]) return forms[otherAt];
  for (int k = 0; k < count; ++k) {
    if (forms[k][0]) return forms[k];
  }
  return count ? forms[0] : "";
}

// Copies up to `n` bytes of `s` to out + *at, keeping room for the NUL;
// a character that doesn't fit whole is left out, and nothing after it.
// False once `out` is full.
bool put(char* out, size_t size, size_t* at, const char* s, size_t n) {
  if (*at + 1 >= size) return n == 0;
  size_t room = size - 1 - *at;
  if (n <= room) {
    std::memcpy(out + *at, s, n);
    *at += n;
    return true;
  }
  // Back to the start of the character the room ends in.
  size_t cut = room;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  std::memcpy(out + *at, s, cut);
  *at += cut;
  return false;
}

}  // namespace

// ---- the UI's language ----

void setLang(Lang l) { g_lang = static_cast<unsigned>(l) < kLangs ? l : Lang::En; }

Lang lang() { return g_lang; }

const char* tag(Lang l) { return table(l).tag; }

const char* autonym(Lang l) { return table(l).autonym; }

bool langOf(const char* t, Lang* out) {
  if (!t) return false;
  for (unsigned i = 0; i < kLangs; ++i) {
    if (std::strcmp(detail::kTables[i].tag, t) == 0) {
      if (out) *out = static_cast<Lang>(i);
      return true;
    }
  }
  return false;
}

// ---- messages ----

bool plural(S id) {
  const unsigned i = static_cast<unsigned>(id);
  return i < kStrings && (detail::kPluralBits[i / 8] >> (i % 8) & 1);
}

const char* tr(S id) {
  const detail::Table* t = nullptr;
  const char* p = find(id, &t);
  if (!plural(id)) return p;
  return form(p, pluralSet(t->tag), Plural::Other);
}

const char* trn(S id, uint32_t n) {
  const detail::Table* t = nullptr;
  const char* p = find(id, &t);
  if (!plural(id)) return p;
  const Locale& l = localeOf(t->tag);
  return form(p, setOf(l.rule), categoryOf(l.rule, n));
}

// ---- plural rules ----

Plural pluralOf(const char* t, uint64_t n) { return categoryOf(localeOf(t).rule, n); }

uint8_t pluralSet(const char* t) { return setOf(localeOf(t).rule); }

// ---- Arg ----

Arg::Arg(const Arg& o) { *this = o; }

Arg& Arg::operator=(const Arg& o) {
  if (this == &o) return *this;
  if (o.s_ >= o.buf_ && o.s_ < o.buf_ + sizeof(o.buf_)) {
    std::memcpy(buf_, o.buf_, static_cast<size_t>(o.s_ - o.buf_) + std::strlen(o.s_) + 1);
    s_ = buf_ + (o.s_ - o.buf_);
  } else {
    s_ = o.s_;
  }
  return *this;
}

void Arg::setUnsigned(unsigned long long v, int width) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v && n < 20);
  if (width > 20) width = 20;
  while (n < width) tmp[n++] = '0';
  for (int i = 0; i < n; ++i) buf_[i] = tmp[n - 1 - i];
  buf_[n] = 0;
  s_ = buf_;
}

void Arg::setSigned(long long v, int width) {
  if (v >= 0) {
    setUnsigned(static_cast<unsigned long long>(v), width);
    return;
  }
  setUnsigned(0ull - static_cast<unsigned long long>(v), width);
  std::memmove(buf_ + 1, buf_, std::strlen(buf_) + 1);
  buf_[0] = '-';
}

Arg Arg::digits(long long v, int width) {
  Arg a;
  a.setSigned(v, width);
  return a;
}

Arg grouped(uint64_t n, const char* t) {
  Arg a, d;
  d.setUnsigned(n, 0);
  const char* digits = d.buf_;
  const int len = static_cast<int>(std::strlen(digits));
  const Locale& l = localeOf(t ? t : tag(g_lang));
  const size_t gl = std::strlen(l.group);
  const bool group = len >= 3 + l.minGroup;
  size_t w = 0;
  for (int i = 0; i < len; ++i) {
    if (group && i > 0 && (len - i) % 3 == 0) {
      std::memcpy(a.buf_ + w, l.group, gl);
      w += gl;
    }
    a.buf_[w++] = digits[i];
  }
  a.buf_[w] = 0;
  a.s_ = a.buf_;
  return a;
}

Arg decimal(double v, int places, const char* t) {
  Arg a;
  if (places < 0) places = 0;
  if (places > 3) places = 3;
  // printf's rounding, so English reads as it always has.
  char tmp[sizeof(a.buf_)];
  const int n = std::snprintf(tmp, sizeof(tmp), "%.*f", places, v);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmp)) return a;
  const Locale& l = localeOf(t ? t : tag(g_lang));
  size_t w = 0;
  for (int i = 0; i < n && w + 3 < sizeof(a.buf_); ++i) {
    if (tmp[i] == '.') {
      for (const char* d = l.decimal; *d; ++d) a.buf_[w++] = *d;
    } else {
      a.buf_[w++] = tmp[i];
    }
  }
  a.buf_[w] = 0;
  a.s_ = a.buf_;
  return a;
}

Arg fixed(long long scaled, int places, const char* t) {
  if (places <= 0) return Arg::digits(scaled, 0);
  if (places > 3) places = 3;
  // Its digits, at least one before the mark: 5 hundredths is "0.05".
  Arg a = Arg::digits(scaled < 0 ? -scaled : scaled, places + 1);
  const size_t len = std::strlen(a.buf_);
  const Locale& l = localeOf(t ? t : tag(g_lang));
  const size_t dl = std::strlen(l.decimal);
  Arg out;
  size_t w = 0;
  if (scaled < 0) out.buf_[w++] = '-';
  std::memcpy(out.buf_ + w, a.buf_, len - places);
  w += len - places;
  std::memcpy(out.buf_ + w, l.decimal, dl);
  w += dl;
  std::memcpy(out.buf_ + w, a.buf_ + len - places, places);
  w += places;
  out.buf_[w] = 0;
  out.s_ = out.buf_;
  return out;
}

// ---- filling a message ----

size_t formatArgs(char* out, size_t size, const char* pattern, const Arg* args, int count) {
  if (!out || size == 0) return 0;
  size_t at = 0;
  const char* p = pattern ? pattern : "";
  while (*p) {
    if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}' && p[1] - '0' < count) {
      const char* s = args[p[1] - '0'].str();
      if (!put(out, size, &at, s, std::strlen(s))) break;
      p += 3;
      continue;
    }
    // A run up to the next brace.
    const char* q = p + 1;
    while (*q && *q != '{') ++q;
    if (!put(out, size, &at, p, static_cast<size_t>(q - p))) break;
    p = q;
  }
  out[at] = 0;
  return at;
}

// ---- a message's lines ----

int lineCount(const char* text) {
  int n = 1;
  for (const char* p = text ? text : ""; *p; ++p) n += *p == '\n';
  return n;
}

size_t line(const char* text, int k, char* out, size_t size) {
  if (!out || size == 0) return 0;
  const char* p = text ? text : "";
  for (int i = 0; i < k; ++i) {
    p = std::strchr(p, '\n');
    if (!p) {
      out[0] = 0;
      return 0;
    }
    ++p;
  }
  const char* end = std::strchr(p, '\n');
  size_t at = 0;
  put(out, size, &at, p, end ? static_cast<size_t>(end - p) : std::strlen(p));
  out[at] = 0;
  return at;
}

}  // namespace uistr
