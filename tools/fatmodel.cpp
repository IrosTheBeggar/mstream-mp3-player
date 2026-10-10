// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
//
// fatmodel: FatFs R0.15 (configured as the Core2 builds it) on a RAM disk,
// counting the card reads per lookup and per walk on a card tree, with and
// without lib/core's SectorCache (docs/METADATA.md 3.2.7, row N8 of 6.1).
// tools/fatmodel.py builds it with the host's gcc and g++ and feeds it
// tools/synthcard.py's card; run that, not this.
//
//   fatmodel LISTING [--cluster BYTES] [--gb N] [--caches 64,128,256]
//            [--seed S] [--probe REL]... [--json 1]
//
// LISTING: one line per folder ("D\t<rel>") or file ("F\t<rel>\t<bytes>"),
// relative to the card's root, UTF-8, in the order a PC copies them (each
// folder before what it holds). The card is formatted FAT32 with 2 FATs,
// the tree created in that order (each file's clusters allocated, its bytes
// left zero), then measured:
// - a lookup: f_open of every audio file once (FA_READ: the decoder's and
//   the scanner's opens), in a random order, stock (every disk_read a card
//   read, as the SD driver does) and through caches of each size, warm;
// - the probes' opens (L0's open bench: the first file of an album whose
//   artist sits at a given place in /music);
// - the walk (3.2.3: a folder at a time, by its path) and today's
//   (LocalStorage::forEachFile: nested readdir), from a fresh mount;
// - the scan's reads: every audio file in the walk's order, opened and its
//   head read (4 KB from 0), from a fresh mount.
// Times use the research's figures: 0.6-1.0 ms a card read (single-sector
// CMD17s, calibrated on the device's card), 35 us a cached sector. With
// --json 1 it prints the figures as one JSON object instead of the text.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "FatModel.h"
#include "SectorCache.h"

namespace {

using fatmodel::Meter;
using fatmodel::Reads;
using fatmodel::drive;
using fatmodel::path;

bool g_text = true;

void say(const char* fmt, ...) {
  if (!g_text) return;
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
}

// The figures as JSON: "key": value pairs, objects opened and closed.
class Json {
public:
  void open(const char* key) {
    sep();
    if (key) out_ += quote(key) + ": ";
    out_ += "{";
    first_ = true;
  }
  void close() {
    out_ += "}";
    first_ = false;
  }
  void num(const char* key, double v) {
    sep();
    char b[48];
    std::snprintf(b, sizeof(b), ": %.6g", v);
    out_ += quote(key) + b;
  }
  void spread(const char* key, double mean, uint64_t p50, uint64_t p90, uint64_t p99, uint64_t max) {
    open(key);
    num("mean", mean);
    num("p50", static_cast<double>(p50));
    num("p90", static_cast<double>(p90));
    num("p99", static_cast<double>(p99));
    num("max", static_cast<double>(max));
    close();
  }
  const std::string& str() const { return out_; }

private:
  static std::string quote(const std::string& k) {
    std::string q = "\"";
    for (char c : k) {
      if (c == '"' || c == '\\') q += '\\';
      q += c;
    }
    return q + "\"";
  }
  void sep() {
    if (!first_) out_ += ", ";
    first_ = false;
  }
  std::string out_;
  bool first_ = true;
};

struct Entry {
  bool dir;
  std::string rel;
  uint32_t size;
};

bool isAudio(const std::string& rel) {
  const size_t slash = rel.rfind('/');
  if (rel.compare(0, 6, "music/") != 0 || rel[slash + 1] == '.') return false;
  const size_t dot = rel.rfind('.');
  if (dot == std::string::npos || dot < slash) return false;
  std::string ext = rel.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == "mp3" || ext == "flac" || ext == "opus";
}

struct Spread {
  double mean = 0;
  uint64_t p50 = 0, p90 = 0, p99 = 0, max = 0, sum = 0;
};
Spread spread(std::vector<uint64_t> v) {
  Spread s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  for (uint64_t x : v) s.sum += x;
  s.mean = static_cast<double>(s.sum) / v.size();
  s.p50 = v[v.size() / 2];
  s.p90 = v[v.size() * 9 / 10];
  s.p99 = v[v.size() * 99 / 100];
  s.max = v.back();
  return s;
}

// The research's time for card reads and cache hits: low and high, in s.
void seconds(const Reads& r, double* lo, double* hi) {
  *lo = (r.card * 0.6 + r.hits * 0.035) / 1000.0;
  *hi = (r.card * 1.0 + r.hits * 0.035) / 1000.0;
}

std::vector<uint64_t> openAll(const std::vector<std::string>& files, const std::vector<uint32_t>& order,
                              Reads* total) {
  std::vector<uint64_t> card;
  card.reserve(order.size());
  FIL f;
  Meter m(0);
  for (uint32_t i : order) {
    m.reset();
    if (f_open(&f, path(0, files[i]).c_str(), FA_READ) != FR_OK) {
      std::fprintf(stderr, "fatmodel: can't open %s\n", files[i].c_str());
      std::exit(2);
    }
    f_close(&f);
    const Reads r = m.read();
    card.push_back(r.card);
    if (total) *total += r;
  }
  return card;
}

std::vector<uint32_t> parseList(const char* s) {
  std::vector<uint32_t> v;
  while (*s) {
    v.push_back(static_cast<uint32_t>(std::strtoul(s, nullptr, 10)));
    const char* c = std::strchr(s, ',');
    if (!c) break;
    s = c + 1;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: fatmodel LISTING [--cluster BYTES] [--gb N] [--caches 64,128,256] [--seed S] [--probe REL]... "
                 "[--json 1]\n");
    return 2;
  }
  uint32_t cluster = 32768, gb = 64, seed = 1;
  std::vector<uint32_t> caches{64, 128, 256};
  std::vector<std::string> probes;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--cluster") cluster = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (k == "--gb") gb = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (k == "--caches") caches = parseList(argv[i + 1]);
    else if (k == "--seed") seed = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (k == "--probe") probes.push_back(argv[i + 1]);
    else if (k == "--json") g_text = std::strcmp(argv[i + 1], "1") != 0;
  }
  Json js;
  js.open(nullptr);

  std::vector<Entry> listing;
  {
    std::ifstream in(argv[1], std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() < 3) continue;
      Entry e;
      e.dir = line[0] == 'D';
      const size_t t2 = line.find('\t', 2);
      e.rel = line.substr(2, t2 == std::string::npos ? std::string::npos : t2 - 2);
      e.size = t2 == std::string::npos ? 0 : static_cast<uint32_t>(std::strtoul(line.c_str() + t2 + 1, nullptr, 10));
      listing.push_back(e);
    }
  }
  if (listing.empty()) {
    std::fprintf(stderr, "fatmodel: an empty listing\n");
    return 2;
  }

  // The card.
  static fatmodel::RamDisk disk(gb * 2097152u);
  static FATFS fs;
  drive(0).disk = &disk;
  if (fatmodel::format(0, cluster) != FR_OK || fatmodel::mount(0, &fs) != FR_OK) {
    std::fprintf(stderr, "fatmodel: can't format a %u GB card with %u B clusters\n", gb, cluster);
    return 2;
  }
  std::vector<std::string> audio;
  uint32_t nFiles = 0, nDirs = 0;
  FIL f;
  for (const Entry& e : listing) {
    FRESULT r;
    if (e.dir) {
      r = f_mkdir(path(0, e.rel).c_str());
      ++nDirs;
    } else {
      r = f_open(&f, path(0, e.rel).c_str(), FA_CREATE_NEW | FA_WRITE);
      if (r == FR_OK && e.size) r = f_expand(&f, e.size, 1);  // contiguous, as a copy to a fresh card
      if (r == FR_OK) r = f_close(&f);
      ++nFiles;
      if (isAudio(e.rel)) audio.push_back(e.rel);
    }
    if (r != FR_OK) {
      std::fprintf(stderr, "fatmodel: %s failed (%d)\n", e.rel.c_str(), static_cast<int>(r));
      return 2;
    }
  }
  uint32_t musicEntries = 0;
  const uint32_t musicSectors = fatmodel::folderSectors(0, "music", &musicEntries);
  say("The card: FAT32, %u GB, %u KB clusters (%u sectors); %u files and %u folders, %zu audio files to open.\n",
              gb, cluster / 1024, fs.csize, nFiles, nDirs, audio.size());
  say("/music: %u directory entries in %u sectors.\n\n", musicEntries, musicSectors);
  js.num("files", nFiles);
  js.num("folders", nDirs);
  js.num("audio", static_cast<double>(audio.size()));
  js.num("music_entries", musicEntries);
  js.num("music_sectors", musicSectors);

  // ---- a lookup ----
  std::mt19937 rng(seed);
  std::vector<uint32_t> order(audio.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  Reads stockTotal;
  const Spread stock = spread(openAll(audio, order, &stockTotal));
  say("A lookup: f_open of each audio file once, in a random order (card reads per open)\n");
  say("  %-22s %8s %6s %6s %6s %6s   %s\n", "", "mean", "p50", "p90", "p99", "max", "ms per open");
  say("  %-22s %8.1f %6llu %6llu %6llu %6llu   %.0f-%.0f\n", "stock (no cache)", stock.mean,
              (unsigned long long)stock.p50, (unsigned long long)stock.p90, (unsigned long long)stock.p99,
              (unsigned long long)stock.max, stock.mean * 0.6, stock.mean * 1.0);
  js.open("lookup");
  js.spread("stock", stock.mean, stock.p50, stock.p90, stock.p99, stock.max);
  SectorCache cache;
  const uint32_t warm = std::min<uint32_t>(2000, static_cast<uint32_t>(order.size() / 4));
  for (uint32_t entries : caches) {
    if (!cache.begin(entries)) continue;
    drive(0).cache = &cache;
    std::shuffle(order.begin(), order.end(), rng);
    Reads t;
    std::vector<uint64_t> per = openAll(audio, order, &t);
    const Spread s = spread(std::vector<uint64_t>(per.begin() + warm, per.end()));
    const double hitsPerOpen = static_cast<double>(t.hits) / per.size();
    char label[40];
    std::snprintf(label, sizeof(label), "%u sectors (%u KB)", entries, entries / 2);
    say("  %-22s %8.2f %6llu %6llu %6llu %6llu   %.1f-%.1f\n", label, s.mean, (unsigned long long)s.p50,
                (unsigned long long)s.p90, (unsigned long long)s.p99, (unsigned long long)s.max,
                s.mean * 0.6 + hitsPerOpen * 0.035, s.mean * 1.0 + hitsPerOpen * 0.035);
    js.spread(std::to_string(entries).c_str(), s.mean, s.p50, s.p90, s.p99, s.max);
    drive(0).cache = nullptr;
  }
  js.close();
  say("  (cached: warm, the first %u opens left out; the research's model: about 56 a lookup, 51 in /music)\n\n",
              warm);

  js.open("probes");
  if (!probes.empty()) {
    say("The probes (L0's open bench), stock card reads per open:\n");
    for (const std::string& p : probes) {
      auto it = std::find(audio.begin(), audio.end(), p);
      if (it == audio.end()) {
        say("  %s: not on the card\n", p.c_str());
        continue;
      }
      const std::vector<uint32_t> one{static_cast<uint32_t>(it - audio.begin())};
      const std::vector<uint64_t> r = openAll(audio, one, nullptr);
      say("  %-60s %4llu\n", p.c_str(), (unsigned long long)r[0]);
      js.num(p.c_str(), static_cast<double>(r[0]));
    }
    say("\n");
  }
  js.close();

  // ---- the walks, from a fresh mount ----
  say("The walk, from a fresh mount (card reads; s at 0.6-1.0 ms a card read and 35 us a hit)\n");
  auto walk = [&](uint32_t entries, bool nested) {
    if (entries) {
      cache.begin(entries);
      drive(0).cache = &cache;
    }
    f_mount(nullptr, "0:", 0);
    fatmodel::mount(0, &fs);
    fatmodel::WalkStats ws;
    const FRESULT r = nested ? fatmodel::walkNested(0, "music", &ws)
                             : fatmodel::walkFolders(0, "music", &ws, [](const std::string&) {});
    drive(0).cache = nullptr;
    double lo, hi;
    seconds(ws.reads, &lo, &hi);
    char label[48];
    if (entries) std::snprintf(label, sizeof(label), "%u-sector cache", entries);
    else std::snprintf(label, sizeof(label), "stock");
    say("  %-34s %-16s %9llu reads, %8llu hits  %6.1f-%.1f s  (%llu folders)%s\n",
                nested ? "today's (nested readdir)" : "3.2.3's (a folder at a time)", label,
                (unsigned long long)ws.reads.card, (unsigned long long)ws.reads.hits, lo, hi,
                (unsigned long long)ws.folders, r == FR_OK ? "" : " FAILED");
    js.num(entries ? std::to_string(entries).c_str() : "stock", r == FR_OK ? static_cast<double>(ws.reads.card) : -1);
  };
  js.open("walk_nested");
  walk(0, true);
  for (uint32_t entries : caches) walk(entries, true);
  js.close();
  js.open("walk");
  walk(0, false);
  for (uint32_t entries : caches) walk(entries, false);
  js.close();
  say("  (the research: about 148,000 sectors and 89-148 s stock, 9-11 s with the cache)\n\n");

  // ---- the scan's reads: the walk's order, open and a 4 KB head ----
  say("The scan's reads: each audio file in the walk's order, opened and 4 KB read from 0 (card reads per file)\n");
  std::vector<std::string> walkOrder;
  {
    fatmodel::WalkStats ws;
    fatmodel::walkFolders(0, "music", &ws, [&](const std::string& rel) {
      if (isAudio(rel)) walkOrder.push_back(rel);
    });
  }
  std::vector<uint8_t> head(4096);
  auto scan = [&](uint32_t entries) {
    if (entries) {
      cache.begin(entries);
      drive(0).cache = &cache;
    }
    f_mount(nullptr, "0:", 0);
    fatmodel::mount(0, &fs);
    Meter m(0);
    for (const std::string& rel : walkOrder) {
      if (f_open(&f, path(0, rel).c_str(), FA_READ) != FR_OK) continue;
      UINT got = 0;
      f_read(&f, head.data(), static_cast<UINT>(head.size()), &got);
      f_close(&f);
    }
    const Reads r = m.read();
    drive(0).cache = nullptr;
    double lo, hi;
    seconds(r, &lo, &hi);
    char label[48];
    if (entries) std::snprintf(label, sizeof(label), "%u-sector cache", entries);
    else std::snprintf(label, sizeof(label), "stock");
    say("  %-22s %6.2f card reads a file (%llu, %llu hits)  %6.1f-%.1f s for the reads\n", label,
                walkOrder.empty() ? 0.0 : static_cast<double>(r.card) / walkOrder.size(), (unsigned long long)r.card,
                (unsigned long long)r.hits, lo, hi);
    js.num(entries ? std::to_string(entries).c_str() : "stock",
           walkOrder.empty() ? 0.0 : static_cast<double>(r.card) / walkOrder.size());
  };
  js.open("scan_per_file");
  scan(0);
  for (uint32_t entries : caches) scan(entries);
  js.close();
  say("  (on the stubs: a head under 4 KB ends in a single-sector read; a real file's 4 KB is one bypassed read)\n");
  js.close();
  if (!g_text) std::printf("%s\n", js.str().c_str());
  return 0;
}
