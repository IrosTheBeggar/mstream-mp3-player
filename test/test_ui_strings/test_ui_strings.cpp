// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the UI's strings (lib/core/UiStrings, docs/I18N.md phase
// 3): the tables read back as the catalogue has them (catalogue.h, made by
// tools/gen_ui_strings.py from i18n/en.json), CLDR 46's plural rules for
// the ten languages against CLDR's own samples, the {0} formatter, the
// numbers as each language writes them, and every message of every
// language in its room, through TextFit with the firmware's own fonts and
// CJK store: English and each translation must fit (but where its
// catalogue says "overflow"), the pseudo-locale en-XA (a third longer) is
// a report, never a failure. The languages' names in themselves in their
// rows. With UI_STRINGS_REPORT set to a path, the overflows of every
// language but English go there as JSON.
// Run: pio test -e native -f test_ui_strings
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CjkGlyphs16.h"
#include "GlyphStore.h"
#include "TextFit.h"
#include "TextFold.h"
#include "UiStrings.h"
#include "UiText.h"
#include "catalogue.h"

// The fonts' data, as the firmware has it (src/ui/VlwFonts.cpp: arrays only).
#include "../../src/ui/VlwFonts.cpp"

using uistr::Arg;
using uistr::Lang;
using uistr::Plural;
using uistr::S;

void setUp() { uistr::setLang(Lang::En); }
void tearDown() { uistr::setLang(Lang::En); }

namespace {

// ---- the fonts, as ui/Fonts measures (test_ui_library has the same) ----

const glyphstore::Store& cjk() {
  static const glyphstore::Store store = [] {
    glyphstore::Store s;
    s.open(kCjkGlyphs16, kCjkGlyphs16Size);
    return s;
  }();
  return store;
}

struct Vlw {
  std::vector<std::pair<uint32_t, int>> advance;  // code point, xAdvance
  int space = 0;                                   // what a missing glyph measures
  bool boldCjk = false;
  explicit Vlw(const uint8_t* d) : boldCjk(d == kVlwSansBold16 || d == kVlwSansBold22) {
    auto be32 = [&](size_t at) {
      return static_cast<uint32_t>(d[at]) << 24 | static_cast<uint32_t>(d[at + 1]) << 16 |
             static_cast<uint32_t>(d[at + 2]) << 8 | d[at + 3];
    };
    const uint32_t n = be32(0);
    const int lineH = static_cast<int>(be32(16)) + static_cast<int>(be32(20));
    space = std::max(static_cast<int>(be32(8)), lineH) * 2 / 7;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t g = 24 + static_cast<size_t>(i) * 28;
      advance.push_back({be32(g), static_cast<int>(be32(g + 12))});
    }
  }
  int find(uint32_t cp) const {
    for (const auto& a : advance) {
      if (a.first == cp) return a.second;
    }
    return -1;
  }
  bool has(uint32_t cp) const { return find(cp) >= 0 || cjk().contains(cp); }
  bool hasAll(const char* s) const {
    for (const char* p = s; *p;) {
      if (!has(textfold::decode(p))) return false;
    }
    return true;
  }
  int width(const char* s) const {
    int w = 0;
    for (const char* p = s; *p;) {
      const uint32_t cp = textfold::decode(p);
      int a = find(cp);
      if (a < 0) {
        a = cjk().advance(cp);
        if (boldCjk) a = glyphstore::boldAdvance(a);
      }
      w += a >= 0 ? a : space;
    }
    return w;
  }
};

const Vlw& font(catalogue::Font f) {
  static const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  switch (f) {
    case catalogue::Font::Small: return small;
    case catalogue::Font::Bold: return bold;
    case catalogue::Font::Title: return title;
    default: return body;
  }
}

int vlwWidth(void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); }
bool vlwHas(void* ctx, uint32_t cp) { return static_cast<const Vlw*>(ctx)->has(cp); }

textfit::Font viewOf(const Vlw& v) {
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&v);
  f.width = vlwWidth;
  f.has = vlwHas;
  return f;
}

// ---- the catalogue's entries ----

const catalogue::Entry* entryOf(S id) {
  for (const auto& e : catalogue::kEntries) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

// A placeholder's worst fill in the UI's language: a count as the code
// writes it (grouped or its digits), a decimal, a name, or another
// message (a plural one at its own worst count).
std::string fill(const catalogue::Placeholder& p, int depth = 0);

// `e`'s text for the count `n` (its forms by the UI's language's rules),
// filled at worst; the count placeholder with `n`.
std::string render(const catalogue::Entry& e, uint32_t n, int depth = 0) {
  const char* pattern = e.plural ? uistr::trn(e.id, n) : uistr::tr(e.id);
  std::vector<std::string> fills(4);
  for (int k = 0; k < 4; ++k) {
    if (e.ph[k].type == catalogue::Fill::None) continue;
    if (k == e.count) {
      fills[k] = e.ph[k].type == catalogue::Fill::Grouped ? uistr::grouped(n).str() : Arg(n).str();
    } else {
      fills[k] = fill(e.ph[k], depth);
    }
  }
  char out[512];
  uistr::format(out, sizeof(out), pattern, fills[0].c_str(), fills[1].c_str(), fills[2].c_str(), fills[3].c_str());
  return out;
}

uint32_t worstCount(const catalogue::Entry& e) {
  if (e.count < 0) return 2;
  return static_cast<uint32_t>(e.ph[e.count].value);
}

std::string fill(const catalogue::Placeholder& p, int depth) {
  using catalogue::Fill;
  switch (p.type) {
    case Fill::Grouped: return uistr::grouped(static_cast<uint64_t>(p.value)).str();
    case Fill::Digits: return Arg(static_cast<long long>(p.value)).str();
    case Fill::Decimal: return uistr::decimal(p.value, p.places).str();
    case Fill::Text: return p.text;
    case Fill::Message: {
      const catalogue::Entry* m = entryOf(p.message);
      if (!m || depth > 2) return "";
      return render(*m, worstCount(*m), depth + 1);
    }
    case Fill::None: break;
  }
  return "";
}

// The counts a plural message is tried with: its forms' typical ones, up
// to its worst.
std::vector<uint32_t> countsFor(const catalogue::Entry& e) {
  if (!e.plural) return {0};
  const uint32_t worst = worstCount(e);
  std::vector<uint32_t> out;
  for (uint32_t n : {0u, 1u, 2u, 3u, 5u, 11u, 21u, 22u, 25u, 101u, 1000u, 1001u, 1002u, 1005u}) {
    if (n <= worst) out.push_back(n);
  }
  out.push_back(worst);
  return out;
}

// ---- does it fit (as TextFit draws it) ----

// Every character of `s` has a glyph as drawn: composed (NFD as NFC), none
// folded to a look-alike or '?' (TextFit::prepare()'s "folded").
bool drawable(const textfit::Font& f, const char* s) {
  char out[1024];
  return !textfit::prepare(f, s, std::strlen(s), out, sizeof(out)).folded;
}

// A line's width as drawn (TextFit: composed, in pieces), -1 when a glyph
// is missing from the font and the store.
int drawnWidth(const Vlw& v, const char* s) {
  const textfit::Font f = viewOf(v);
  return drawable(f, s) ? textfit::drawnWidth(f, s) : -1;
}

struct Fit {
  bool ok = true;
  int width = 0;     // the widest line, in the font it would be drawn in
  std::string why;   // what doesn't fit
};

// One line in the room's font, else its other font. A glyph the fonts
// lack doesn't fit (the width then is the font's own sum).
bool lineFits(const catalogue::Entry& e, const char* line, int px, int* width) {
  const Vlw& f = font(e.font);
  const int w = drawnWidth(f, line);
  *width = w >= 0 ? w : f.width(line);
  if (w >= 0 && w <= px) return true;
  if (e.elseFont != catalogue::Font::None) {
    const int g = drawnWidth(font(e.elseFont), line);
    if (g >= 0 && g <= px) {
      *width = g;
      return true;
    }
  }
  return false;
}

Fit fits(const catalogue::Entry& e, const std::string& text) {
  Fit r;
  using catalogue::Layout;
  if (e.layout == Layout::Wrap) {
    // textfit::wrap over the room's lines, none cut.
    const Vlw& f = font(e.font);
    std::vector<char> lines(static_cast<size_t>(e.lines + 1) * 160);
    const int n = textfit::wrap(viewOf(f), text.c_str(), text.size(), e.px, e.lines + 1, lines.data(), 160);
    for (int i = 0; i < n; ++i) r.width = std::max(r.width, textfit::drawnWidth(viewOf(f), lines.data() + i * 160));
    if (n > e.lines || !drawable(viewOf(f), text.c_str())) {
      r.ok = false;
      r.why = std::to_string(n) + " lines of " + std::to_string(e.lines);
    }
    return r;
  }
  if (e.layout == Layout::Toast) {
    // One Body line; else the what (Small, before ": ") over the rest
    // (Body, else Small), each its two-line room.
    int w = 0;
    const catalogue::Entry body{e.id, e.name, e.plural, e.one, e.other, catalogue::Font::Body,
                                catalogue::Font::None, e.px, 1, Layout::Line, 0, e.count, false, false, false, {}};
    if (lineFits(body, text.c_str(), e.px, &w)) {
      r.width = w;
      return r;
    }
    r.width = w;
    const size_t colon = text.find(": ");
    if (colon == std::string::npos) {
      r.ok = false;
      r.why = "one line, and no \": \" to split at";
      return r;
    }
    const std::string what = text.substr(0, colon), rest = text.substr(colon + 2);
    const Vlw& small = font(catalogue::Font::Small);
    const int drawnWhat = drawnWidth(small, what.c_str());
    const int ww = drawnWhat >= 0 ? drawnWhat : small.width(what.c_str());
    const catalogue::Entry restRoom{e.id, e.name, e.plural, e.one, e.other, catalogue::Font::Body,
                                    catalogue::Font::Small, e.twoLinePx, 1, Layout::Line, 0, e.count, false, false, false, {}};
    int rw = 0;
    const bool restOk = lineFits(restRoom, rest.c_str(), e.twoLinePx, &rw);
    r.width = std::max(ww, rw);
    if (drawnWhat < 0 || ww > e.twoLinePx || !restOk) {
      r.ok = false;
      r.why = "two lines: " + std::to_string(ww) + " and " + std::to_string(rw) + " px";
    }
    return r;
  }
  // Lines placed one by one ('\n' between them).
  const int count = uistr::lineCount(text.c_str());
  if (count > e.lines) {
    r.ok = false;
    r.why = std::to_string(count) + " lines of " + std::to_string(e.lines);
  }
  for (int k = 0; k < count; ++k) {
    char line[256];
    uistr::line(text.c_str(), k, line, sizeof(line));
    int w = 0;
    if (!lineFits(e, line, e.px, &w)) {
      r.ok = false;
      r.why = "line " + std::to_string(k + 1) + ": " + std::to_string(w) + " px" +
              (w <= e.px ? ", a character the fonts lack" : "");
    }
    r.width = std::max(r.width, w);
  }
  return r;
}

const char* fontName(catalogue::Font f) {
  switch (f) {
    case catalogue::Font::Body: return "Body";
    case catalogue::Font::Small: return "Small";
    case catalogue::Font::Bold: return "Bold";
    case catalogue::Font::Title: return "Title";
    case catalogue::Font::None: break;
  }
  return "";
}

std::string jsonString(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c == '\n') {
      o += "\\n";
    } else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

struct Overflow {
  std::string id, lang, text, why;
  const catalogue::Entry* e;
  int width;
  uint32_t n;
};

// A message the UI's language's catalogue says overflows (until a layout
// fix): English's "overflow" marks count for every language (the room is
// too small already), a translation's own only for it.
bool markedIn(const char* tag, const catalogue::Entry& e) {
  if (e.knownOverflow) return true;
  for (const auto& m : catalogue::kLangOverflows) {
    if (m.tag[0] && std::strcmp(m.tag, tag) == 0 && m.id == e.id) return true;
  }
  return false;
}

bool hasCatalogueFile(const char* tag) {
  for (const char* t : catalogue::kCatalogues) {
    if (std::strcmp(t, tag) == 0) return true;
  }
  return false;
}

// UI_STRINGS_REPORT (a path): the overflows as JSON, for the layout fixes.
void writeReport(const std::vector<Overflow>& over, int rooms) {
  const char* path = std::getenv("UI_STRINGS_REPORT");
  if (!path || !path[0]) return;
  FILE* f = std::fopen(path, "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::fprintf(f, "{\n  \"what\": \"Messages that overflow their room (test_ui_strings), by language\",\n");
  std::fprintf(f, "  \"rooms\": %d,\n  \"overflows\": [\n", rooms);
  for (size_t i = 0; i < over.size(); ++i) {
    const Overflow& o = over[i];
    std::fprintf(f,
                 "    {\"id\": %s, \"lang\": %s, \"font\": \"%s\", \"px\": %d, \"lines\": %d, \"width\": %d, "
                 "\"count\": %lu, \"why\": %s, \"text\": %s}%s\n",
                 jsonString(o.id).c_str(), jsonString(o.lang).c_str(), fontName(o.e->font), o.e->px, o.e->lines,
                 o.width, static_cast<unsigned long>(o.n), jsonString(o.why).c_str(), jsonString(o.text).c_str(),
                 i + 1 < over.size() ? "," : "");
  }
  std::fprintf(f, "  ]\n}\n");
  std::fclose(f);
}

// Every entry with a room, in the UI's language: what doesn't fit.
std::vector<Overflow> overflows() {
  std::vector<Overflow> out;
  const char* tag = uistr::tag(uistr::lang());
  for (const auto& e : catalogue::kEntries) {
    if (e.font == catalogue::Font::None || e.optional) continue;  // (optional: another message when it doesn't)
    for (uint32_t n : countsFor(e)) {
      const std::string text = render(e, n);
      const Fit f = fits(e, text);
      if (!f.ok) {
        out.push_back({e.name, tag, text, f.why, &e, f.width, n});
        break;  // one count is enough to say so
      }
    }
  }
  return out;
}

}  // namespace

// ---- the tables read back as the catalogue says ----

void test_english_is_the_catalogue() {
  for (const auto& e : catalogue::kEntries) {
    TEST_ASSERT_EQUAL_MESSAGE(e.plural, uistr::plural(e.id), e.name);
    if (e.plural) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.one, uistr::trn(e.id, 1), e.name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.other, uistr::trn(e.id, 2), e.name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.other, uistr::trn(e.id, 0), e.name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.other, uistr::tr(e.id), e.name);
    } else {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.one, uistr::tr(e.id), e.name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.one, uistr::trn(e.id, 1), e.name);
    }
  }
  // Every id has an entry (and its text).
  for (unsigned i = 0; i < uistr::kStrings; ++i) TEST_ASSERT_NOT_NULL(entryOf(static_cast<S>(i)));
  // Out of range: nothing, not a crash.
  TEST_ASSERT_EQUAL_STRING("", uistr::tr(static_cast<S>(uistr::kStrings)));
}

void test_the_languages() {
  // The ten (the mobile app's and the terminal player's), English first,
  // in their order, each by its name in itself; then the pseudo-locale.
  static const char* const kTags[] = {"en", "de", "es", "fr", "it", "ja", "pl", "pt", "ru", "zh", "en-XA"};
  static const char* const kNames[] = {"English",
                                       "Deutsch",
                                       "Espa\xC3\xB1ol",
                                       "Fran\xC3\xA7"
                                       "ais",
                                       "Italiano",
                                       "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                       "Polski",
                                       "Portugu\xC3\xAAs",
                                       "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",
                                       "\xE4\xB8\xAD\xE6\x96\x87"};
  TEST_ASSERT_EQUAL_UINT(11, uistr::kLangs);
  for (unsigned i = 0; i < uistr::kLangs; ++i) {
    const Lang l = static_cast<Lang>(i);
    TEST_ASSERT_EQUAL_STRING(kTags[i], uistr::tag(l));
    if (i < 10) TEST_ASSERT_EQUAL_STRING(kNames[i], uistr::autonym(l));
    Lang back = Lang::En;
    TEST_ASSERT_TRUE(uistr::langOf(kTags[i], &back));
    TEST_ASSERT_TRUE(back == l);
    // A catalogue where i18n/ has its file (English's and the pseudo's
    // always).
    TEST_ASSERT_EQUAL_MESSAGE(i == 10 || hasCatalogueFile(kTags[i]), uistr::hasCatalogue(l), kTags[i]);
  }
  // One with no catalogue yet draws English: every message, its plural
  // forms by English's rule; translated() says none is its own.
  for (unsigned i = 1; i < 10; ++i) {
    const Lang l = static_cast<Lang>(i);
    if (uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    for (const auto& e : catalogue::kEntries) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e.one, e.plural ? uistr::trn(e.id, 1) : uistr::tr(e.id), e.name);
      TEST_ASSERT_FALSE_MESSAGE(uistr::translated(e.id), e.name);
    }
  }
  uistr::setLang(Lang::En);
  TEST_ASSERT_TRUE(uistr::translated(S::language_machine_note));
  TEST_ASSERT_FALSE(uistr::translated(static_cast<S>(uistr::kStrings)));
  TEST_ASSERT_EQUAL_STRING("en", uistr::tag(Lang::En));
  TEST_ASSERT_EQUAL_STRING("English", uistr::autonym(Lang::En));
  Lang l = Lang::EnXA;
  TEST_ASSERT_TRUE(uistr::langOf("en", &l));
  TEST_ASSERT_TRUE(l == Lang::En);
  TEST_ASSERT_TRUE(uistr::langOf("en-XA", &l));
  TEST_ASSERT_TRUE(l == Lang::EnXA);
  TEST_ASSERT_FALSE(uistr::langOf("xx", &l));
  TEST_ASSERT_FALSE(uistr::langOf(nullptr, &l));
  uistr::setLang(Lang::EnXA);
  TEST_ASSERT_TRUE(uistr::lang() == Lang::EnXA);
  uistr::setLang(static_cast<Lang>(200));  // not a language: English
  TEST_ASSERT_TRUE(uistr::lang() == Lang::En);
}

// ---- the plural rules (CLDR 46: common/supplemental/plurals.xml) ----

void test_plural_sets_are_the_generators() {
  for (const auto& l : catalogue::kLangSets) {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(l.set, uistr::pluralSet(l.tag), l.tag);
  }
}

namespace {
struct Samples {
  const char* tag;
  Plural cat;
  std::vector<uint64_t> n;
};
// CLDR 46's @integer samples, each range written out (1c6 = 1,000,000).
const std::vector<uint64_t> kOtherEn = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 100, 1000, 10000, 100000,
                                        1000000};
const std::vector<uint64_t> kMillions = {1000000, 2000000, 3000000, 4000000, 5000000, 6000000};
const std::vector<uint64_t> kOtherEs = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 100, 1000, 10000,
                                        100000, 2000, 3000, 4000, 5000, 6000};
const std::vector<uint64_t> kOtherFr = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 100, 1000, 10000,
                                        100000, 2000, 3000, 4000, 5000, 6000};
const std::vector<uint64_t> kFew = {2, 3, 4, 22, 23, 24, 32, 33, 34, 42, 43, 44, 52, 53, 54, 62, 102, 1002};
const std::vector<uint64_t> kManySlavic = {0, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 100, 1000, 10000,
                                           100000, 1000000};
const std::vector<uint64_t> kOtherCjk = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 100, 1000, 10000, 100000,
                                         1000000};
}  // namespace

void test_plural_rules_match_cldr() {
  const std::vector<Samples> all = {
      {"en", Plural::One, {1}},
      {"en", Plural::Other, kOtherEn},
      {"de", Plural::One, {1}},
      {"de", Plural::Other, kOtherEn},
      {"es", Plural::One, {1}},
      {"es", Plural::Many, kMillions},
      {"es", Plural::Other, kOtherEs},
      {"fr", Plural::One, {0, 1}},
      {"fr", Plural::Many, kMillions},
      {"fr", Plural::Other, kOtherFr},
      {"it", Plural::One, {1}},
      {"it", Plural::Many, kMillions},
      {"it", Plural::Other, kOtherEs},
      {"ja", Plural::Other, kOtherCjk},
      {"pl", Plural::One, {1}},
      {"pl", Plural::Few, kFew},
      {"pl", Plural::Many, kManySlavic},
      {"pt", Plural::One, {0, 1}},
      {"pt", Plural::Many, kMillions},
      {"pt", Plural::Other, kOtherFr},
      {"ru", Plural::One, {1, 21, 31, 41, 51, 61, 71, 81, 101, 1001}},
      {"ru", Plural::Few, kFew},
      {"ru", Plural::Many, kManySlavic},
      {"zh", Plural::Other, kOtherCjk},
  };
  for (const auto& s : all) {
    for (uint64_t n : s.n) {
      char msg[48];
      snprintf(msg, sizeof(msg), "%s %llu", s.tag, static_cast<unsigned long long>(n));
      TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(s.cat), static_cast<int>(uistr::pluralOf(s.tag, n)), msg);
    }
  }
  // Beyond the samples: the teens and the hundreds.
  TEST_ASSERT_TRUE(uistr::pluralOf("ru", 11) == Plural::Many);
  TEST_ASSERT_TRUE(uistr::pluralOf("ru", 111) == Plural::Many);
  TEST_ASSERT_TRUE(uistr::pluralOf("ru", 112) == Plural::Many);
  TEST_ASSERT_TRUE(uistr::pluralOf("ru", 121) == Plural::One);
  TEST_ASSERT_TRUE(uistr::pluralOf("ru", 122) == Plural::Few);
  TEST_ASSERT_TRUE(uistr::pluralOf("pl", 21) == Plural::Many);  // Polish's one is 1 alone
  TEST_ASSERT_TRUE(uistr::pluralOf("pl", 112) == Plural::Many);
  TEST_ASSERT_TRUE(uistr::pluralOf("pl", 122) == Plural::Few);
  TEST_ASSERT_TRUE(uistr::pluralOf("fr", 1000001) == Plural::Other);
  TEST_ASSERT_TRUE(uistr::pluralOf("xx", 1) == Plural::One);  // an unknown tag: English's
  TEST_ASSERT_TRUE(uistr::pluralOf("en-XA", 2) == Plural::Other);
  // Every count of the device (up to 99,999 tracks) picks a category the
  // language has.
  for (const auto& l : catalogue::kLangSets) {
    for (uint64_t n = 0; n < 100000; n += (n < 2000 ? 1 : 97)) {
      const unsigned c = static_cast<unsigned>(uistr::pluralOf(l.tag, n));
      TEST_ASSERT_TRUE_MESSAGE(l.set & (1u << c), l.tag);
    }
  }
}

// A plural message picks its form by the rule; a form the catalogue left
// out (empty) is the "other" one. English's tables: one and other.
void test_plural_forms() {
  TEST_ASSERT_EQUAL_STRING("Shuffling {0} track", uistr::trn(S::toast_shuffling, 1));
  TEST_ASSERT_EQUAL_STRING("Shuffling {0} tracks", uistr::trn(S::toast_shuffling, 0));
  TEST_ASSERT_EQUAL_STRING("Shuffling {0} tracks", uistr::trn(S::toast_shuffling, 21));
  char b[64];
  uistr::trnf(b, sizeof(b), S::queue_cap_push_added, 1, uistr::grouped(1));
  TEST_ASSERT_EQUAL_STRING("Added", b);
  uistr::trnf(b, sizeof(b), S::queue_cap_push_added, 1234, uistr::grouped(1234));
  TEST_ASSERT_EQUAL_STRING("Added 1,234 tracks", b);
  // A message that isn't plural: trn() is tr().
  TEST_ASSERT_EQUAL_STRING(uistr::tr(S::toast_undone), uistr::trn(S::toast_undone, 5));
}

// ---- filling a message ----

void test_format() {
  char b[64];
  TEST_ASSERT_EQUAL_size_t(13, uistr::format(b, sizeof(b), "{0} of {1}!", 4999, 5000u));
  TEST_ASSERT_EQUAL_STRING("4999 of 5000!", b);
  // Reordered, repeated, past the arguments (kept), other braces (kept).
  uistr::format(b, sizeof(b), "{1}, {0}, {1}", "a", "b");
  TEST_ASSERT_EQUAL_STRING("b, a, b", b);
  uistr::format(b, sizeof(b), "{0} {2} {x} {10} {", "a");
  TEST_ASSERT_EQUAL_STRING("a {2} {x} {10} {", b);
  uistr::format(b, sizeof(b), "100% {0}%", 5);
  TEST_ASSERT_EQUAL_STRING("100% 5%", b);  // '%' is a percent sign
  uistr::format(b, sizeof(b), "");
  TEST_ASSERT_EQUAL_STRING("", b);
  uistr::format(b, sizeof(b), nullptr);
  TEST_ASSERT_EQUAL_STRING("", b);
  uistr::format(b, sizeof(b), "{0}|{1}", static_cast<const char*>(nullptr), -42);
  TEST_ASSERT_EQUAL_STRING("|-42", b);
  // The numbers' types the firmware passes (uint32_t is unsigned long there).
  uistr::format(b, sizeof(b), "{0} {1} {2} {3}", static_cast<uint8_t>(7), static_cast<uint16_t>(65535),
                4294967295ul, 18446744073709551615ull);
  TEST_ASSERT_EQUAL_STRING("7 65535 4294967295 18446744073709551615", b);
  uistr::format(b, sizeof(b), "{0}:{1}", Arg::digits(5, 2), Arg::digits(-3, 3));
  TEST_ASSERT_EQUAL_STRING("05:-003", b);
  // Bounded: cut at a character boundary, never through a UTF-8 sequence.
  char six[6];
  TEST_ASSERT_EQUAL_size_t(5, uistr::format(six, sizeof(six), "abcdefgh"));
  TEST_ASSERT_EQUAL_STRING("abcde", six);
  TEST_ASSERT_EQUAL_size_t(5, uistr::format(six, sizeof(six), "abc{0}", "\xC3\xA9\xC3\xA9"));
  TEST_ASSERT_EQUAL_STRING("abc\xC3\xA9", six);
  TEST_ASSERT_EQUAL_size_t(5, uistr::format(six, sizeof(six), "ab\xE2\x80\xA6x"));  // "ab…" is 5 bytes ...
  TEST_ASSERT_EQUAL_STRING("ab\xE2\x80\xA6", six);
  char five[5];
  TEST_ASSERT_EQUAL_size_t(2, uistr::format(five, sizeof(five), "ab\xE2\x80\xA6x"));  // ... no room for "…"
  TEST_ASSERT_EQUAL_STRING("ab", five);
  char one[1];
  TEST_ASSERT_EQUAL_size_t(0, uistr::format(one, sizeof(one), "abc"));
  TEST_ASSERT_EQUAL_STRING("", one);
  // An Arg copied keeps its own text (a number's digits live in it).
  const Arg a = 1234;
  const Arg copy = a;
  TEST_ASSERT_EQUAL_STRING("1234", copy.str());
  TEST_ASSERT_TRUE(copy.str() != a.str());
  // trf() and trnf().
  uistr::trf(b, sizeof(b), S::coach_header_tip, 1, 2);
  TEST_ASSERT_EQUAL_STRING("Tip 1 of 2", b);
  uistr::trnf(b, sizeof(b), S::library_tracks_n, 1, 1);
  TEST_ASSERT_EQUAL_STRING("1 track", b);
}

void test_lines() {
  TEST_ASSERT_EQUAL_INT(1, uistr::lineCount(""));
  TEST_ASSERT_EQUAL_INT(1, uistr::lineCount(nullptr));
  TEST_ASSERT_EQUAL_INT(2, uistr::lineCount(uistr::tr(S::coach_hold_play)));
  char b[32];
  TEST_ASSERT_EQUAL_size_t(10, uistr::line(uistr::tr(S::coach_hold_play), 0, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("speaker or", b);
  uistr::line(uistr::tr(S::coach_hold_play), 1, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("headphones", b);
  TEST_ASSERT_EQUAL_size_t(0, uistr::line(uistr::tr(S::coach_hold_play), 2, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
  uistr::line("volume up", 0, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("volume up", b);
  char four[4];
  uistr::line("abcdef\nx", 0, four, sizeof(four));
  TEST_ASSERT_EQUAL_STRING("abc", four);
}

// ---- numbers as each language writes them (CLDR 46) ----

void test_numbers() {
  struct Case {
    const char* tag;
    uint64_t n;
    const char* want;
  };
  const Case cases[] = {
      {"en", 0, "0"},
      {"en", 999, "999"},
      {"en", 1000, "1,000"},
      {"en", 19410, "19,410"},
      {"en", 4294967295ull, "4,294,967,295"},
      {"de", 1234, "1.234"},
      {"de", 19410, "19.410"},
      {"es", 1234, "1234"},  // minimumGroupingDigits 2
      {"es", 19410, "19.410"},
      {"fr", 1234, "1\xC2\xA0" "234"},  // U+00A0 (CLDR: U+202F, which the fonts lack)
      {"fr", 1234567, "1\xC2\xA0" "234\xC2\xA0" "567"},
      {"it", 1234, "1234"},
      {"it", 12345, "12.345"},
      {"ja", 19410, "19,410"},
      {"pl", 1234, "1234"},
      {"pl", 19410, "19\xC2\xA0" "410"},
      {"pt", 1234, "1.234"},
      {"ru", 1234, "1\xC2\xA0" "234"},
      {"zh", 1234, "1,234"},
      {"xx", 1234, "1,234"},  // an unknown tag: English's
  };
  for (const Case& c : cases) TEST_ASSERT_EQUAL_STRING_MESSAGE(c.want, uistr::grouped(c.n, c.tag).str(), c.tag);
  // The UI's language by default.
  TEST_ASSERT_EQUAL_STRING("19,410", uistr::grouped(19410).str());
  // Decimals: printf's rounding, the language's mark.
  TEST_ASSERT_EQUAL_STRING("31.9", uistr::decimal(31.914, 1).str());
  TEST_ASSERT_EQUAL_STRING("31,9", uistr::decimal(31.914, 1, "de").str());
  TEST_ASSERT_EQUAL_STRING("0.0", uistr::decimal(0.0, 1).str());
  TEST_ASSERT_EQUAL_STRING("2", uistr::decimal(2.4, 0).str());
  TEST_ASSERT_EQUAL_STRING("4.15", uistr::fixed(415, 2).str());
  TEST_ASSERT_EQUAL_STRING("0.05", uistr::fixed(5, 2).str());
  TEST_ASSERT_EQUAL_STRING("37,8", uistr::fixed(378, 1, "ru").str());
  TEST_ASSERT_EQUAL_STRING("2.5", uistr::fixed(25, 1, "ja").str());
  TEST_ASSERT_EQUAL_STRING("-1.5", uistr::fixed(-15, 1).str());
  TEST_ASSERT_EQUAL_STRING("7", uistr::fixed(7, 0).str());
}

// ---- the pseudo-locale ----

void test_pseudo_locale() {
  uistr::setLang(Lang::EnXA);
  TEST_ASSERT_EQUAL_STRING("en-XA", uistr::tag(uistr::lang()));
  for (const auto& e : catalogue::kEntries) {
    const char* t = e.plural ? uistr::trn(e.id, 2) : uistr::tr(e.id);
    // Bracketed, every line, and longer than the English.
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('[', t[0], e.name);
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(t) > std::strlen(e.plural ? e.other : e.one), e.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(uistr::lineCount(e.plural ? e.other : e.one), uistr::lineCount(t), e.name);
  }
  // The placeholders as they were.
  char b[96];
  uistr::trf(b, sizeof(b), S::coach_header_tip, 1, 2);
  TEST_ASSERT_TRUE(std::strstr(b, " 1 ") != nullptr);
  TEST_ASSERT_TRUE(std::strstr(b, " 2") != nullptr);
}

// ---- every message in its room ----

// The rooms that are UiText's constants say the same number.
void test_rooms_are_the_codes() {
  for (const auto& c : catalogue::kConsts) {
    if (!c.name[0]) continue;
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.code, c.px, c.name);
  }
}

// Every language with a catalogue, every message in every room it has, as
// TextFit draws it with the firmware's fonts and CJK store (composed, no
// glyph missing), its placeholders at their worst. A real language
// (English, and each translation as it lands in i18n/) must fit, but where
// its catalogue says "overflow" (a layout fix to come; English's marks
// count for all), and each of its own marks must still overflow (else the
// mark goes). The pseudo-locale (accented, a third longer) is where a
// longer language won't fit: a report (UI_STRINGS_REPORT: JSON), never a
// failure. A language with no catalogue yet draws English: nothing more.
void test_every_language_fits_its_rooms() {
  std::vector<Overflow> report;  // every language's but English's
  int bad = 0;
  int rooms = 0;
  for (const auto& e : catalogue::kEntries) rooms += e.font != catalogue::Font::None;
  for (unsigned i = 0; i < uistr::kLangs; ++i) {
    const Lang l = static_cast<Lang>(i);
    const char* tag = uistr::tag(l);
    if (!uistr::hasCatalogue(l)) {
      printf("%s: no catalogue yet (its texts are English's)\n", tag);
      continue;
    }
    uistr::setLang(l);
    const std::vector<Overflow> over = overflows();
    if (l == Lang::EnXA) {
      printf("%s: %d of %d rooms overflow (the pseudo-locale: a report)\n", tag, static_cast<int>(over.size()), rooms);
      report.insert(report.end(), over.begin(), over.end());
      continue;
    }
    int known = 0;
    for (const Overflow& o : over) {
      const bool marked = markedIn(tag, *o.e);
      printf("%s: %s overflow: %s: \"%s\" (%s %d px, %d lines): %s\n", tag, marked ? "known" : "NEW", o.id.c_str(),
             o.text.c_str(), fontName(o.e->font), o.e->px, o.e->lines, o.why.c_str());
      ++(marked ? known : bad);
      if (l != Lang::En) report.push_back(o);
    }
    // Each of its own marks does overflow (English's: in English).
    for (const auto& e : catalogue::kEntries) {
      const bool own = l == Lang::En ? e.knownOverflow : markedIn(tag, e) && !e.knownOverflow;
      if (!own) continue;
      bool any = false;
      for (const Overflow& o : over) any = any || o.id == e.name;
      if (!any) {
        printf("%s: marked, but fits: %s\n", tag, e.name);
        ++bad;
      }
    }
    printf("%s: %d messages overflow as its catalogue says\n", tag, known);
  }
  uistr::setLang(Lang::En);
  writeReport(report, rooms);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, bad, "overflows a catalogue doesn't say (or says and it fits): above");
}

// The languages' names in themselves (never translated): each in its row
// on the Language page (Body, Bold for the one in use) and as the line
// under Output's "Language" (Small), every glyph in the fonts or the store.
void test_autonyms_fit() {
  struct Room {
    catalogue::Font font;
    int px;
  };
  const Room rooms[] = {{catalogue::Font::Body, uitext::kLangRowW},
                        {catalogue::Font::Bold, uitext::kLangRowW},
                        {catalogue::Font::Small, uitext::kLangLineW}};
  for (unsigned i = 0; i < uistr::kLangs; ++i) {
    const char* a = uistr::autonym(static_cast<Lang>(i));
    for (const Room& r : rooms) {
      const int w = drawnWidth(font(r.font), a);
      TEST_ASSERT_TRUE_MESSAGE(w > 0, a);
      TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(r.px, w, a);
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_english_is_the_catalogue);
  RUN_TEST(test_the_languages);
  RUN_TEST(test_plural_sets_are_the_generators);
  RUN_TEST(test_plural_rules_match_cldr);
  RUN_TEST(test_plural_forms);
  RUN_TEST(test_format);
  RUN_TEST(test_lines);
  RUN_TEST(test_numbers);
  RUN_TEST(test_pseudo_locale);
  RUN_TEST(test_rooms_are_the_codes);
  RUN_TEST(test_every_language_fits_its_rooms);
  RUN_TEST(test_autonyms_fit);
  return UNITY_END();
}
