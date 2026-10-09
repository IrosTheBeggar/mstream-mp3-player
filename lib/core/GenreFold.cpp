// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "GenreFold.h"

#include "Fnv1a.h"
#include "TextFold.h"

namespace dance {

namespace {

// Where the key's characters go: an optional buffer (cut to cap - 1) and the
// running FNV-1a 32, while counting the whole key's length.
struct KeySink {
  char* out = nullptr;
  size_t cap = 0;
  size_t length = 0;
  uint32_t hash = kFnvBasis;

  void put(char c) {
    if (out && length + 1 < cap) out[length] = c;
    ++length;
    const uint8_t b = static_cast<uint8_t>(c);
    hash = fnv1a32(&b, 1, hash);
  }
  void put(const char* s) {
    while (*s) put(*s++);
  }
};

// B.8's steps 2-6 over the ASCII that step 1 produces, one character at a
// time.
class Folder {
 public:
  explicit Folder(KeySink& sink) : sink_(sink) {}

  void feed(char c) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');  // step 2
    if (c == '&' || c == '+') {                                       // step 3: " and "
      endWord();
      for (const char* w = "and"; *w; ++w) wordChar(*w);
      endWord();
      return;
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {  // step 4
      wordChar(c);
    } else {
      endWord();
    }
  }

  void finish() {
    endWord();
    if (pendingN_) sink_.put('n');  // step 5: no word after it
    pendingN_ = false;
  }

 private:
  void wordChar(char c) {
    if (!inWord_) {
      // A new word: a waiting "n" had a word after it.
      if (pendingN_) sink_.put(nHadWordBefore_ ? "and" : "n");
      pendingN_ = false;
      inWord_ = true;
      wordLen_ = 0;
    }
    if (wordLen_ == 0 && c == 'n') {
      heldN_ = true;  // the word may be exactly "n": hold it
    } else {
      if (heldN_) sink_.put('n');
      heldN_ = false;
      sink_.put(c);
    }
    ++wordLen_;
  }

  void endWord() {
    if (!inWord_) return;
    if (heldN_ && wordLen_ == 1) {
      pendingN_ = true;  // exactly "n": it waits for the next word
      nHadWordBefore_ = anyWord_;
    }
    heldN_ = false;
    anyWord_ = true;
    inWord_ = false;
  }

  KeySink& sink_;
  bool inWord_ = false;
  bool heldN_ = false;           // the current word is "n" so far
  bool pendingN_ = false;        // a finished word "n" waits for the next word
  bool nHadWordBefore_ = false;  // ...and a word came before it
  bool anyWord_ = false;         // a word has finished
  uint32_t wordLen_ = 0;
};

void fold(const char* tag, KeySink& sink) {
  Folder folder(sink);
  if (tag) {
    const char* s = tag;
    while (*s) {
      const uint32_t cp = textfold::decode(s);
      if (cp == 0) break;
      if (cp < 0x80) {
        folder.feed(static_cast<char>(cp));
        continue;
      }
      const char* r = textfold::replacement(cp, textfold::Mode::Full);
      for (; r && *r; ++r) folder.feed(*r);  // step 1
    }
  }
  folder.finish();
}

}  // namespace

size_t foldGenre(const char* tag, char* out, size_t cap) {
  KeySink sink;
  sink.out = out;
  sink.cap = cap;
  fold(tag, sink);
  if (out && cap > 0) out[sink.length < cap ? sink.length : cap - 1] = '\0';
  return sink.length;
}

bool genreKey(const char* tag, uint32_t* key) {
  KeySink sink;
  fold(tag, sink);
  if (sink.length == 0) return false;
  if (key) *key = sink.hash;
  return true;
}

}  // namespace dance
