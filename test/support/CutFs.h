// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// A card in memory for TagStore's file interface (lib/core/TagStore.h) that
// can lose its power at any write, sync, remove or rename (docs/METADATA.md
// 2.17, item 6), modelled as FatFs does them:
//
//   - a directory entry names a cluster chain; FA_CREATE_ALWAYS on an
//     existing file, f_truncate and f_unlink free the chain's clusters;
//   - f_rename is two directory writes, the new entry first: a cut between
//     them leaves two entries on one chain;
//   - freeing a chain another entry still uses is the corruption 2.12.6
//     guards against: it is recorded as a violation, which no test allows;
//   - f_write and f_sync are one step each; f_unlink is one; f_rename two;
//     an open with FA_CREATE_ALWAYS one; f_close syncs a written file.
//
// A cut at step k: the steps before it happened, step k didn't, and every
// step and read after it fails (the card is gone). reboot() then gives the
// card as the next boot finds it, three ways:
//   - InOrder: every step before the cut on the card, synced or not;
//   - LoseUnsynced: a file's writes since its last sync lost (the directory
//     entry still says the old size);
//   - Torn: as InOrder, and the cut write half done.
// diskCheck() copies shared chains apart, as chkdsk does. Host only.
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "TagStore.h"

namespace cutfs {

enum class Variant { InOrder, LoseUnsynced, Torn };

class CutFs : public tagstore::Fs {
public:
  struct Chain {
    std::vector<uint8_t> data;
    std::vector<uint8_t> synced;
  };

  // The step the power goes at (1-based); -1: never.
  long cutAt = -1;
  long steps = 0;
  bool dead = false;
  std::vector<std::string> violations;
  // What went through it.
  uint64_t bytesRead = 0, bytesWritten = 0;
  uint32_t opens = 0, peakOpen = 0, openNow = 0;

  std::map<std::string, int> dir;
  std::map<int, Chain> chains;
  int nextChain = 1;

  class F : public tagstore::File {
  public:
    F(CutFs* fs, const std::string& name, int chain, bool writable)
        : fs_(fs), name_(name), chain_(chain), writable_(writable) {}
    uint32_t size() const override {
      if (fs_->dead) return 0;
      auto it = fs_->chains.find(chain_);
      return it == fs_->chains.end() ? 0 : static_cast<uint32_t>(it->second.data.size());
    }
    bool read(uint32_t offset, void* out, uint32_t n) override {
      if (fs_->dead) return false;
      auto it = fs_->chains.find(chain_);
      if (it == fs_->chains.end()) return false;
      const auto& d = it->second.data;
      if (offset > d.size() || n > d.size() - offset) return false;
      if (n) std::memcpy(out, d.data() + offset, n);
      fs_->bytesRead += n;
      return true;
    }
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      if (!writable_) return false;
      auto it = fs_->chains.find(chain_);
      if (it == fs_->chains.end()) return false;
      const bool cutHere = !fs_->dead && fs_->steps + 1 == fs_->cutAt;
      if (!fs_->step()) {
        if (!cutHere) return false;
        fs_->torn_ = true;
        fs_->tornChain_ = chain_;
        fs_->tornAt_ = offset;
        fs_->tornData_.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
        return false;
      }
      auto& d = it->second.data;
      if (offset + static_cast<size_t>(n) > d.size()) d.resize(offset + static_cast<size_t>(n), 0xEE);
      if (n) std::memcpy(d.data() + offset, data, n);
      fs_->bytesWritten += n;
      dirty_ = true;
      return true;
    }
    bool sync() override {
      if (!writable_) return !fs_->dead;
      if (!fs_->step()) return false;
      auto it = fs_->chains.find(chain_);
      if (it != fs_->chains.end()) it->second.synced = it->second.data;
      dirty_ = false;
      return true;
    }
    bool truncate(uint32_t size) override {
      if (!writable_) return false;
      auto it = fs_->chains.find(chain_);
      if (it == fs_->chains.end() || size > it->second.data.size()) return false;
      if (!fs_->step()) return false;
      if (size < it->second.data.size()) fs_->freeing(chain_, name_, "truncate");
      it->second.data.resize(size);
      dirty_ = true;
      return true;
    }

  private:
    friend class CutFs;
    CutFs* fs_;
    std::string name_;
    int chain_;
    bool writable_;
    bool dirty_ = false;
  };

  // ---- tagstore::Fs ----
  tagstore::File* open(const char* path, Mode mode) override {
    if (dead) return nullptr;
    const std::string p = path;
    auto it = dir.find(p);
    if (mode == Mode::Create) {
      if (!step()) return nullptr;
      if (it != dir.end()) {
        freeing(it->second, p, "create");
        dropRef(it->second, p);
      }
      const int c = nextChain++;
      chains[c] = Chain();
      dir[p] = c;
      return track(new F(this, p, c, true));
    }
    if (it == dir.end()) return nullptr;
    return track(new F(this, p, it->second, mode == Mode::Update));
  }
  bool close(tagstore::File* file) override {
    F* f = static_cast<F*>(file);
    bool ok = true;
    if (f->dirty_) ok = f->sync();
    --openNow;
    delete f;
    return ok && !dead;
  }
  bool exists(const char* path) override { return !dead && dir.count(path) > 0; }
  bool remove(const char* path) override {
    if (dead) return false;
    auto it = dir.find(path);
    if (it == dir.end()) return false;
    if (!step()) return false;
    const int c = it->second;
    freeing(c, path, "remove");
    dir.erase(it);
    if (!referenced(c)) chains.erase(c);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    if (dead) return false;
    auto it = dir.find(from);
    if (it == dir.end() || dir.count(to)) return false;
    const int c = it->second;
    if (!step()) return false;
    dir[to] = c;  // the new entry first
    if (!step()) return false;
    dir.erase(from);
    return true;
  }
  uint32_t firstCluster(const char* path) override {
    if (dead) return 0;
    auto it = dir.find(path);
    if (it == dir.end()) return 0;
    return chains[it->second].data.empty() ? 0u : static_cast<uint32_t>(it->second);
  }

  // ---- the tests' ----
  // The card as the next boot finds it.
  CutFs reboot(Variant v) const {
    CutFs n;
    n.dir = dir;
    n.chains = chains;
    n.nextChain = nextChain;
    for (auto& kv : n.chains) {
      if (v == Variant::LoseUnsynced) kv.second.data = kv.second.synced;
      kv.second.synced = kv.second.data;
    }
    if (v == Variant::Torn && torn_ && n.chains.count(tornChain_)) {
      auto& d = n.chains[tornChain_].data;
      const size_t half = tornData_.size() / 2;
      if (tornAt_ + half > d.size()) d.resize(tornAt_ + half, 0xEE);
      if (half) std::memcpy(d.data() + tornAt_, tornData_.data(), half);
      n.chains[tornChain_].synced = d;
    }
    return n;
  }
  // chkdsk: every entry on a chain of its own.
  void diskCheck() {
    std::set<int> seen;
    for (auto& kv : dir) {
      if (seen.insert(kv.second).second) continue;
      const int c = nextChain++;
      chains[c] = chains[kv.second];
      kv.second = c;
    }
  }
  std::vector<uint8_t> bytes(const std::string& p) const {
    auto it = dir.find(p);
    if (it == dir.end()) return {};
    return chains.at(it->second).data;
  }
  void put(const std::string& p, const std::vector<uint8_t>& b) {
    auto it = dir.find(p);
    int c;
    if (it == dir.end()) {
      c = nextChain++;
      dir[p] = c;
    } else {
      c = it->second;
    }
    chains[c].data = b;
    chains[c].synced = b;
  }
  // Two names on one chain, as a rename cut between its directory writes leaves them.
  void link(const std::string& from, const std::string& to) { dir[to] = dir.at(from); }
  bool shared(const std::string& a, const std::string& b) const {
    return dir.count(a) && dir.count(b) && dir.at(a) == dir.at(b);
  }
  std::vector<std::string> names() const {
    std::vector<std::string> v;
    for (auto& kv : dir) v.push_back(kv.first);
    return v;
  }

private:
  bool step() {
    if (dead) return false;
    ++steps;
    if (cutAt >= 0 && steps == cutAt) {
      dead = true;
      return false;
    }
    return true;
  }
  bool referenced(int c) const {
    for (auto& kv : dir)
      if (kv.second == c) return true;
    return false;
  }
  // `name`'s chain is about to be freed: a violation when another entry
  // uses it and it holds clusters.
  void freeing(int c, const std::string& name, const char* how) {
    if (chains[c].data.empty()) return;
    for (auto& kv : dir)
      if (kv.second == c && kv.first != name)
        violations.push_back(std::string(how) + " " + name + " freed the chain " + kv.first + " uses");
  }
  // `name` leaves chain c (FA_CREATE_ALWAYS gives it a new one); the other
  // entries keep theirs.
  void dropRef(int c, const std::string& name) {
    dir.erase(name);
    if (!referenced(c)) chains.erase(c);
  }
  tagstore::File* track(F* f) {
    ++opens;
    ++openNow;
    if (openNow > peakOpen) peakOpen = openNow;
    return f;
  }

  bool torn_ = false;
  int tornChain_ = 0;
  uint32_t tornAt_ = 0;
  std::vector<uint8_t> tornData_;
};

}  // namespace cutfs
