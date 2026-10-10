// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// A fake FAT card for CardWalk (lib/core/CardWalk.h): a tree of folders and
// files under /music, each file a size, a FAT time and content made from a
// seed, listed through cardwalk::Lister as f_readdir would: in the order the
// entries were made (a directory's slots), or shuffled. Host only: std
// containers.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "CardContainer.h"
#include "CardContract.h"
#include "CardWalk.h"

namespace fakefat {

namespace cc = cardcontract;

// Byte i of a file made from `seed` (seed 0: byte i is i & 0xFF, 2.18's qfp
// pattern).
inline uint8_t byteAt(uint32_t seed, uint32_t i) {
  if (seed == 0) return static_cast<uint8_t>(i & 0xFF);
  uint32_t x = seed * 0x9E3779B1u ^ (i * 0x85EBCA77u + 0x165667B1u);
  x ^= x >> 15;
  x *= 0x2C1B3C6Du;
  x ^= x >> 12;
  x *= 0x297A2D39u;
  x ^= x >> 15;
  return static_cast<uint8_t>(x);
}

struct File {
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint32_t seed = 1;
  uint32_t slot = 0;  // when it was made: the directory order
};

inline std::string parentOf(const std::string& rel) {
  const size_t s = rel.rfind('/');
  return s == std::string::npos ? std::string() : rel.substr(0, s);
}
inline std::string leafOf(const std::string& rel) {
  const size_t s = rel.rfind('/');
  return s == std::string::npos ? rel : rel.substr(s + 1);
}

class FileSource : public cc::Source {
public:
  FileSource() = default;
  FileSource(uint32_t size, uint32_t seed) : size_(size), seed_(seed) {}
  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (offset > size_ || n > size_ - offset) return false;
    uint8_t* o = static_cast<uint8_t*>(out);
    for (uint32_t i = 0; i < n; ++i) o[i] = byteAt(seed_, offset + i);
    ++reads;
    return true;
  }
  uint32_t reads = 0;

private:
  uint32_t size_ = 0;
  uint32_t seed_ = 0;
};

class Card : public cardwalk::Lister {
public:
  // ---- the tree ----
  void addFile(const std::string& rel, uint32_t size, uint32_t fatTime, uint32_t seed = 1) {
    addFolder(parentOf(rel));
    File f;
    f.size = size;
    f.fatTime = fatTime;
    f.seed = seed;
    auto it = files.find(rel);
    if (it == files.end()) {
      f.slot = nextSlot_++;
      ++shape_;
    } else {
      f.slot = it->second.slot;
    }
    files[rel] = f;
  }
  void addFolder(const std::string& rel) {
    for (std::string p = rel; !p.empty(); p = parentOf(p)) {
      if (folders.count(p)) break;
      folders[p] = nextSlot_++;
      ++shape_;
    }
  }
  // A file, or a folder and everything under it.
  void remove(const std::string& rel) {
    ++shape_;
    files.erase(rel);
    const std::string pre = rel + "/";
    for (auto it = files.begin(); it != files.end();)
      it = it->first.compare(0, pre.size(), pre) == 0 ? files.erase(it) : std::next(it);
    for (auto it = folders.begin(); it != folders.end();)
      it = it->first == rel || it->first.compare(0, pre.size(), pre) == 0 ? folders.erase(it) : std::next(it);
  }
  // A folder renamed (FAT keeps its files' stamps).
  void renameFolder(const std::string& from, const std::string& to) {
    ++shape_;
    const std::string pre = from + "/";
    std::map<std::string, File> f2;
    for (const auto& kv : files) {
      if (kv.first.compare(0, pre.size(), pre) == 0)
        f2[to + "/" + kv.first.substr(pre.size())] = kv.second;
      else
        f2[kv.first] = kv.second;
    }
    files.swap(f2);
    std::map<std::string, uint32_t> d2;
    for (const auto& kv : folders) {
      if (kv.first == from)
        d2[to] = kv.second;
      else if (kv.first.compare(0, pre.size(), pre) == 0)
        d2[to + "/" + kv.first.substr(pre.size())] = kv.second;
      else
        d2[kv.first] = kv.second;
    }
    folders.swap(d2);
    addFolder(parentOf(to));
  }
  File& at(const std::string& rel) { return files.at(rel); }
  // Listing orders from a seed; 0: the order the entries were made.
  void shuffle(uint32_t seed) {
    shuffle_ = seed;
    ++shape_;
  }

  // The file's qfp (2.3.5), from its content.
  uint64_t qfpOf(const std::string& rel) const {
    const File& f = files.at(rel);
    const cc::QfpRanges r = cc::qfpRanges(f.size);
    std::vector<uint8_t> head(r.headBytes), tail(r.tailBytes);
    for (uint32_t i = 0; i < r.headBytes; ++i) head[i] = byteAt(f.seed, i);
    for (uint32_t i = 0; i < r.tailBytes; ++i) tail[i] = byteAt(f.seed, r.tailOffset + i);
    return cc::qfp(f.size, head.data(), tail.data());
  }

  std::map<std::string, File> files;       // relative to /music
  std::map<std::string, uint32_t> folders;  // their slots ("" isn't one: /music always exists)
  bool noMusic = false;                     // no /music folder at all

  // ---- faults and counts ----
  int failOpenAt = -1;  // the n-th openDir() fails (0: the first)
  int failNextAt = -1;  // the n-th next() fails
  uint32_t opens = 0, nexts = 0, fileOpens = 0, maxOpen = 0;
  uint32_t openNow = 0;
  std::vector<std::string> opened;  // every folder listed, in order

  // ---- cardwalk::Lister ----
  Open openDir(const char* rel, size_t len) override {
    const std::string p(rel, len);
    if (std::strlen(rel) != len) return Open::Error;  // the walk's paths are NUL-terminated at len
    if (failOpenAt >= 0 && static_cast<int>(opens) == failOpenAt) {
      ++opens;
      return Open::Error;
    }
    ++opens;
    opened.push_back(p);
    if (p.empty() ? noMusic : !folders.count(p)) return Open::Missing;
    index();
    auto it = children_.find(p);
    list_ = it == children_.end() ? nullptr : &it->second;
    pos_ = 0;
    ++openNow;
    maxOpen = std::max(maxOpen, openNow);
    return Open::Ok;
  }
  Next next(cardwalk::Entry* out) override {
    if (failNextAt >= 0 && static_cast<int>(nexts) == failNextAt) {
      ++nexts;
      return Next::Error;
    }
    ++nexts;
    if (!list_ || pos_ >= list_->size()) return Next::End;
    const Listed& l = (*list_)[pos_++];
    out->name = l.name.c_str();
    out->nameLength = l.name.size();
    out->folder = l.file == nullptr;
    out->size = l.file ? l.file->size : 0;  // read now: a test may change a file's fields in place
    out->fatTime = l.file ? l.file->fatTime : 0;
    return Next::Entry;
  }
  void closeDir() override {
    if (openNow) --openNow;
  }
  cc::Source* openFile(const char* rel, size_t len) override {
    const std::string p(rel, len);
    auto it = files.find(p);
    if (it == files.end() || std::strlen(rel) != len) return nullptr;
    ++fileOpens;
    src_ = FileSource(it->second.size, it->second.seed);
    return &src_;
  }
  void closeFile() override {}

private:
  struct Listed {
    std::string name;
    const File* file;  // nullptr: a folder
    uint32_t slot;
  };
  // Each folder's entries in directory order, made again when the tree's
  // shape (or the order) changed: a listing then costs its own entries only.
  void index() {
    if (indexed_ == shape_) return;
    indexed_ = shape_;
    children_.clear();
    for (const auto& kv : folders)
      if (!kv.first.empty()) children_[parentOf(kv.first)].push_back(Listed{leafOf(kv.first), nullptr, kv.second});
    for (const auto& kv : files)
      children_[parentOf(kv.first)].push_back(Listed{leafOf(kv.first), &kv.second, kv.second.slot});
    for (auto& c : children_) {
      std::sort(c.second.begin(), c.second.end(), [&](const Listed& a, const Listed& b) {
        if (shuffle_ == 0) return a.slot < b.slot;
        const uint64_t ha = cc::fnv1a64(a.name.data(), a.name.size(), shuffle_);
        const uint64_t hb = cc::fnv1a64(b.name.data(), b.name.size(), shuffle_);
        return ha != hb ? ha < hb : a.name < b.name;
      });
    }
  }
  std::map<std::string, std::vector<Listed>> children_;
  uint64_t shape_ = 1, indexed_ = 0;
  const std::vector<Listed>* list_ = nullptr;
  size_t pos_ = 0;
  uint32_t shuffle_ = 0;
  uint32_t nextSlot_ = 0;
  FileSource src_;
};

}  // namespace fakefat
