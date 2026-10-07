// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "audio/OpusGenerator.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

// ESP8266Audio's bundled libopus, as its AudioGeneratorOpus.h includes it
// (and nothing else of that file: see the class comment).
#include "libopus/include/opus.h"

namespace {
constexpr int kRate = static_cast<int>(oggopus::kRate);
}  // namespace

size_t OpusGenerator::stateBytes() { return static_cast<size_t>(opus_decoder_get_size(2)); }

void OpusGenerator::layout(size_t sizes[2]) {
  sizes[kStatePart] = stateBytes();
  sizes[kPcmPart] = kPcmBytes;
}

const char* OpusGenerator::placementName(Placement p) {
  switch (p) {
    case Placement::Low: return "the pinned block (PSRAM, its lower 2 MB)";
    case Placement::Internal: return "internal RAM";
    case Placement::High: return "PSRAM above 0x3FA00000";
  }
  return "?";
}

OpusGenerator::OpusGenerator(DecoderArena& arena, int layout) : arena_(arena), layout_(layout) { running = false; }

OpusGenerator::~OpusGenerator() {
  stop();
  heap_caps_free(pageBuf_);
  heap_caps_free(packetBuf_);
  heap_caps_free(scratch_);
}

bool OpusGenerator::allocBuffers() {
  if (pageBuf_) return true;
  pageBuf_ = static_cast<uint8_t*>(heap_caps_malloc(oggopus::kPageBytes, MALLOC_CAP_SPIRAM));
  packetBuf_ = static_cast<uint8_t*>(heap_caps_malloc(oggopus::kMaxPacketBytes, MALLOC_CAP_SPIRAM));
  scratch_ = static_cast<uint8_t*>(heap_caps_malloc(oggopus::kFramePacketBytes, MALLOC_CAP_SPIRAM));
  if (pageBuf_ && packetBuf_ && scratch_) {
    Serial.printf("[opus] buffers: %lu B of PSRAM (the page, the packet, a frame), kept from now on\n",
                  (unsigned long)(oggopus::kPageBytes + oggopus::kMaxPacketBytes + oggopus::kFramePacketBytes));
    return true;
  }
  heap_caps_free(pageBuf_);
  heap_caps_free(packetBuf_);
  heap_caps_free(scratch_);
  pageBuf_ = packetBuf_ = scratch_ = nullptr;
  return false;
}

bool OpusGenerator::open(AudioFileSource* file, bool speaker, const oggopus::OpenRecord* hint) {
  opened_ = false;
  opened_on_ = nullptr;
  fromRecord_ = false;
  refusal_[0] = 0;
  note_[0] = 0;
  if (!file || !file->isOpen()) {
    snprintf(refusal_, sizeof(refusal_), "the file isn't open");
    snprintf(note_, sizeof(note_), "%s", refusal_);
    return false;
  }
  if (!allocBuffers()) {
    snprintf(refusal_, sizeof(refusal_), "no PSRAM for the Opus buffers");
    snprintf(note_, sizeof(note_), "%s", refusal_);
    return false;
  }
  src_.attach(file);
  reader_.emplace(src_, file->getSize(), pageBuf_, packetBuf_);
  reader_->setReadSlice(kPageSlice);  // the audio pages a slice a step (the class comment: the pass's time)
  planned_ = false;  // a new track: from the top unless a plan is set before begin()
  planUs_ = 0;
  landedT_ = -1;
  const int64_t t0 = esp_timer_get_time();
  // From the cache's record when there is one and the file is still the
  // one it describes (one read); else the headers and the exact length
  // (the last page of our stream: Now Playing's length from the open, and
  // gate G9's truth), the tail scan before the first audio page so that
  // page stays in hand for the first pass (the class comment).
  fromRecord_ = hint != nullptr && reader_->openFrom(*hint);
  if (!fromRecord_) {
    const oggopus::Reader::Open o = reader_->open(true);
    if (o != oggopus::Reader::Open::Ok) {
      reader_->refusal(refusal_, sizeof(refusal_));
      reader_->refusalNote(note_, sizeof(note_));
      return false;
    }
  }
  openUs_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
  openReads_ = reader_->pages().reads();
  openBytes_ = reader_->pages().bytesRead();
  speaker_ = speaker;
  opened_on_ = file;
  opened_ = true;
  const oggopus::Head& h = reader_->head();
  char mapping[40] = "";
  if (h.mapped()) {
    snprintf(mapping, sizeof(mapping), " (the mapping table: L = %u, R = %u)", (unsigned)h.map[0], (unsigned)h.map[1]);
  }
  Serial.printf("[opus] open: %u ch%s, pre-skip %u, gain %d, input %lu Hz, g0 %lld, tags %lu B in %lu pages, "
                "length %lu ms (%s), %s%lu reads / %lu B in %lu ms\n",
                (unsigned)h.channels, mapping, (unsigned)h.preSkip,
                (int)h.gain, (unsigned long)h.inputRate, (long long)reader_->g0(), (unsigned long)reader_->tagsBytes(),
                (unsigned long)reader_->tagsPages(), (unsigned long)reader_->lengthMs(),
                reader_->lastGranule() < 0 ? "no last page found: unknown"
                : reader_->chained()       ? "exact; another stream follows: only the first plays"
                                           : "exact, from the last page",
                fromRecord_ ? "from the cache's record, checked in " : "", (unsigned long)openReads_,
                (unsigned long)openBytes_, (unsigned long)(openUs_ / 1000));
  return true;
}

uint32_t OpusGenerator::planStartMs(uint32_t ms, uint32_t prerollMs, oggopus::StartPlan* out) {
  // Between the open and the begin: the page buffer is free (the reader is
  // at the first audio page, nothing read of it yet), which the plan's
  // probes and its read of Q need. The reads are the plan's own; the
  // backend's log line has their count from the plan.
  if (!reader_ || !opened_) {
    *out = oggopus::StartPlan{};
    return 0;
  }
  const int64_t t0 = esp_timer_get_time();
  const uint32_t land = reader_->planStartMs(ms, prerollMs, out);
  planUs_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
  return land;
}

void OpusGenerator::planStart(uint64_t sample, uint32_t prerollSamples, oggopus::StartPlan* out) {
  if (!reader_ || !opened_) {
    *out = oggopus::StartPlan{};
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  reader_->planStart(sample, prerollSamples, out);
  planUs_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
}

void OpusGenerator::setStartPlan(const oggopus::StartPlan* plan) {
  planned_ = plan != nullptr;
  if (plan) plan_ = *plan;
}

void* OpusGenerator::allocHigh(size_t bytes, uint32_t* held) {
  // The heap hands out the lowest fit: blocks held below the line push
  // the next one past it (docs/RESAMPLER.md section 10d measured 720 KB
  // held taking a per-track malloc to 0x3FA00014). Every try is kept
  // until one lands high, then the rest go back.
  constexpr uint32_t kMaxHeld = 32;
  constexpr size_t kChunk = 256 * 1024;
  void* keep[kMaxHeld];
  uint32_t n = 0;
  void* found = nullptr;
  for (;;) {
    void* p = heap_caps_aligned_alloc(DecoderArena::kAlign, bytes, MALLOC_CAP_SPIRAM);
    if (!p) break;
    if (DecoderArena::where(p, bytes) == DecoderArena::Where::PsramHigh) {
      found = p;
      break;
    }
    if (n == kMaxHeld) {
      heap_caps_free(p);
      break;
    }
    keep[n++] = p;
    if (n < kMaxHeld) {
      void* c = heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM);
      if (!c) break;
      keep[n++] = c;
    }
  }
  for (uint32_t i = 0; i < n; ++i) heap_caps_free(keep[i]);
  *held = n;
  return found;
}

bool OpusGenerator::claimState() {
  releaseState();
  size_t sizes[2];
  layout(sizes);
  const size_t bytes = DecoderArena::alignUp(sizes[0]) + DecoderArena::alignUp(sizes[1]);
  highHeld_ = 0;
  if (placement_ == Placement::Internal) {
    own_ = heap_caps_aligned_alloc(DecoderArena::kAlign, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!own_) {
      Serial.printf("[opus] no %lu B block of internal RAM for the state (largest %lu B): the pinned block instead\n",
                    (unsigned long)bytes, (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
  } else if (placement_ == Placement::High) {
    own_ = allocHigh(bytes, &highHeld_);
    if (!own_) Serial.println("[opus] no PSRAM block above 0x3FA00000 to be had: the pinned block instead");
  }
  if (!own_ && layout_ >= 0 && arena_.claim(static_cast<size_t>(layout_))) {
    claimed_ = true;
    dec_ = static_cast<OpusDecoder*>(arena_.part(kStatePart));
    pcm_ = static_cast<int16_t*>(arena_.part(kPcmPart));
    return dec_ && pcm_;
  }
  if (!own_) {
    // No block (none at boot, or lent out): one of this track's own,
    // wherever the heap has room (its speed depends on where that is).
    own_ = heap_caps_aligned_alloc(DecoderArena::kAlign, bytes, MALLOC_CAP_SPIRAM);
    if (!own_) {
      Serial.println("[opus] no RAM for the decoder's state");
      return false;
    }
    Serial.printf("[opus] the state malloc'd for this track (%s)\n",
                  !arena_.attached() ? "no pinned block" : arena_.inUse() ? "the pinned block is in use" : "no layout");
  }
  dec_ = static_cast<OpusDecoder*>(own_);
  pcm_ = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(own_) + DecoderArena::alignUp(sizes[0]));
  return true;
}

void OpusGenerator::releaseState() {
  if (claimed_) arena_.release();
  claimed_ = false;
  heap_caps_free(own_);
  own_ = nullptr;
  dec_ = nullptr;
  pcm_ = nullptr;
}

void OpusGenerator::describeState(char* buf, size_t size) const {
  if (!dec_) {
    snprintf(buf, size, "none (not begun)");
    return;
  }
  size_t sizes[2];
  layout(sizes);
  const size_t bytes = DecoderArena::alignUp(sizes[0]) + DecoderArena::alignUp(sizes[1]);
  const DecoderArena::Where w = DecoderArena::where(dec_, bytes);
  if (claimed_) {
    snprintf(buf, size, "pinned at %p, %s", static_cast<const void*>(dec_), DecoderArena::whereName(w));
  } else if (placement_ == Placement::Internal && w == DecoderArena::Where::Elsewhere) {
    snprintf(buf, size, "at %p, internal RAM (the O knob)", static_cast<const void*>(dec_));
  } else {
    snprintf(buf, size, "malloc'd at %p, %s%s", static_cast<const void*>(dec_), DecoderArena::whereName(w),
             placement_ == Placement::High ? " (the O knob)" : "");
  }
}

bool OpusGenerator::begin(AudioFileSource* source, AudioOutput* output) {
  running = false;
  if (!source || !output) return false;
  if (!opened_ || opened_on_ != source) {
    if (!open(source, speaker_)) return false;
  }
  file = source;
  this->output = output;
  if (!claimState()) return false;
  const oggopus::Head& h = reader_->head();
  channels_ = h.channels;
  mono_ = channels_ == 1;
  mapped_ = h.mapped();
  mapL_ = h.map[0];
  mapR_ = h.map[1];
  if (opus_decoder_init(dec_, kRate, channels_) != OPUS_OK) {
    releaseState();
    return false;
  }
  opus_decoder_ctl(dec_, OPUS_SET_GAIN(static_cast<opus_int32>(h.gain)));
  if (speaker_) opus_decoder_ctl(dec_, OPUS_SET_PHASE_INVERSION_DISABLED(1));
  if (planned_) {
    // A start part of the way in (the class comment): the reader at the
    // plan's page (restart() when it is from the top), the timeline by the
    // plan: packets ending before the preroll are skipped undecoded, the
    // samples before the target dropped. The decoder was initialised
    // fresh just above, the reset such a start needs (Rockbox's 2019 stack
    // overflow came from a missing one). The first next() may say Pending
    // (a slice of the page, a step of a scan): decodeNext() ends the pass
    // on it and the next pass asks again, as from the top.
    reader_->startAt(plan_);
    tl_.start(plan_);
  } else {
    reader_->restart();
    tl_.start(reader_->g0(), h.preSkip);
  }
  landedT_ = -1;
  havePacket_ = false;
  frameIdx_ = 0;
  outAt_ = outLeft_ = 0;
  ended_ = false;
  fillLeft_ = 0;
  fillConceal_ = 0;
  haveToc_ = false;
  gapEnded_ = false;
  pass_.reset();
  packets_ = decodeCalls_ = concealed_ = malformed_ = dropped_ = skipped_ = gaps_ = gapsFilled_ = 0;
  filled_ = 0;
  maxDecodeUs_ = maxLoopUs_ = 0;
  output->begin();
  output->SetRate(kRate);
  output->SetChannels(2);
  running = true;
  return true;
}

bool OpusGenerator::stop() {
  running = false;
  releaseState();
  opened_ = false;
  opened_on_ = nullptr;
  return true;
}

void OpusGenerator::made(int16_t* out, uint32_t n) {
  const oggopus::Timeline::Keep k = tl_.decoded(n);
  outAt_ = k.skip;
  outLeft_ = k.take;
  if (k.take == 0) return;
  // The first kept sample's trimmed index (decoded() moved the timeline
  // past these n): where the start landed, for the end line.
  if (landedT_ < 0) landedT_ = tl_.position() - static_cast<int64_t>(n) + k.skip;
  if (mono_) {
    // Mono was made in the buffer's upper half and is expanded forwards in
    // place (sample i is read before stereo frame i is written).
    for (uint32_t i = k.skip; i < k.skip + k.take; ++i) {
      const int16_t s = out[i];
      pcm_[2 * i] = s;
      pcm_[2 * i + 1] = s;
    }
  } else if (mapped_) {
    // Family 1's table (RFC 7845 section 5.1.1): L = decoded map[0], R =
    // decoded map[1], so {1,0} swaps the channels and {0,0} or {1,1} puts
    // one of them on both sides.
    for (uint32_t i = k.skip; i < k.skip + k.take; ++i) {
      const int16_t l = pcm_[2 * i + mapL_];
      const int16_t r = pcm_[2 * i + mapR_];
      pcm_[2 * i] = l;
      pcm_[2 * i + 1] = r;
    }
  }
}

void OpusGenerator::fillNext() {
  int16_t* out = mono_ ? pcm_ + oggopus::kMaxFrameSamples : pcm_;
  uint32_t n = fillLeft_ > oggopus::kMaxFrameSamples ? oggopus::kMaxFrameSamples : static_cast<uint32_t>(fillLeft_);
  bool concealed = false;
  if (fillConceal_ > 0 && oggopus::frameSamples(fillToc_) <= fillLeft_) {
    // libopus's PLC continues the last frame (its size: the PLC wants a
    // whole number of 2.5 ms, and the frame's size is one; a malformed
    // packet's own TOC when nothing was decoded before it).
    n = oggopus::frameSamples(fillToc_);
    const int64_t t0 = esp_timer_get_time();
    const int got = opus_decode(dec_, nullptr, 0, out, static_cast<int>(n), 0);
    const auto us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    ++decodeCalls_;
    ++passCalls_;
    if (us > maxDecodeUs_) maxDecodeUs_ = us;
    concealed = got == static_cast<int>(n);
    --fillConceal_;
  }
  if (!concealed) {
    fillConceal_ = 0;
    std::memset(out, 0, static_cast<size_t>(n) * (mono_ ? 1 : 2) * sizeof(int16_t));
  }
  fillLeft_ -= n;
  filled_ += n;
  passSamples_ += n;
  made(out, n);
}

bool OpusGenerator::decodeNext() {
  for (;;) {
    if (havePacket_ && fillLeft_ == 0 && (frameIdx_ >= frames_.count || tl_.finished())) {
      tl_.packetDone();  // (the page's granule puts the timeline right; the EOS page sets the end)
      havePacket_ = false;
      if (tl_.finished()) return false;  // the EOS trim reached: nothing after it is audio
    }
    if (havePacket_) break;
    const oggopus::Reader::Next n = reader_->next(&pkt_);
    if (n == oggopus::Reader::Next::End) return false;
    if (n == oggopus::Reader::Next::Pending) {
      // A slice of the next page read, or the reader's share of pages for
      // one call spent with no packet to show (empty pages, another
      // stream's): the pass ends here, with nothing in hand, and the next
      // one asks again.
      passYield_ = true;
      return true;
    }
    ++packets_;
    if (pkt_.gapBefore) ++gaps_;
    tl_.packet(pkt_);
    const bool framed = pkt_.samples > 0 && oggopus::splitPacket(pkt_.data, pkt_.bytes, &frames_);
    if (pkt_.samples < 0) {
      // No TOC to size it by (0 bytes, or an impossible count; libopus
      // would refuse it too): dropped, the timeline put right at the
      // page's end. A few of them end the pass (the cap): a file of
      // nothing but junk packets (a page of 255 empty ones, thousands of
      // pages) would otherwise all go by inside this one call.
      ++dropped_;
      tl_.packetDone();
      if (++passMalformed_ >= kPassMalformed) return true;  // nothing to hand over (outLeft_ is 0): loop() ends the pass
      continue;
    }
    // The gap plan (lib/core/OggOpus: Packet::startK, Timeline::gap()):
    // what a damaged or lost page took is made before this packet, so the
    // count never slips; too much of it ends the track instead.
    const int64_t gap = tl_.gap();
    if (gap > oggopus::kMaxGapSamples) {
      gapEnded_ = true;
      return false;
    }
    fillLeft_ = gap;
    fillConceal_ = 0;
    if (gap > 0) {
      ++gapsFilled_;
      if (haveToc_) {
        fillConceal_ = kFillConceal;
        fillToc_ = lastToc_;
      }
    }
    if (!framed) {
      // Its TOC reads but its framing doesn't (libopus would refuse it):
      // concealed for its TOC's duration, as a frame libopus refuses is,
      // in one fill with the gap before it, so the count runs on as the
      // file says and the page's granule has nothing to put right.
      ++malformed_;
      frames_.count = 0;  // nothing of it to decode after the fill
      fillLeft_ += pkt_.samples;
      if (fillConceal_ == 0) {
        fillConceal_ = kFillConceal;
        fillToc_ = pkt_.data[0];
      }
    }
    havePacket_ = true;
    frameIdx_ = 0;
    break;
  }
  if (fillLeft_ > 0) {
    fillNext();
    return true;
  }
  if (frameIdx_ == 0 && frames_.count > 0 && !tl_.wanted(static_cast<uint32_t>(pkt_.samples))) {
    // Before a plan's preroll (M3: Timeline::start(plan)): skipped by its
    // TOC, undecoded; the loop's top puts the packet by.
    tl_.skipped(static_cast<uint32_t>(pkt_.samples));
    ++skipped_;
    frames_.count = 0;
    return true;
  }
  // One frame, as a code-0 packet of its own (a code-0 packet as it is).
  const uint8_t* data = pkt_.data;
  uint32_t n = pkt_.bytes;
  if (frames_.code() != 0) {
    n = oggopus::framePacket(frames_, frameIdx_, scratch_);
    data = scratch_;
  }
  const uint8_t toc = frames_.toc;
  ++frameIdx_;
  // Mono decodes into the buffer's upper half (made() expands it).
  int16_t* out = mono_ ? pcm_ + oggopus::kMaxFrameSamples : pcm_;
  const int64_t t0 = esp_timer_get_time();
  int got = opus_decode(dec_, data, static_cast<opus_int32>(n), out, static_cast<int>(oggopus::kMaxFrameSamples), 0);
  if (got < 0) {
    ++concealed_;
    got = opus_decode(dec_, nullptr, 0, out, static_cast<int>(oggopus::frameSamples(toc)), 0);
    if (got < 0) got = 0;
  }
  const auto us = static_cast<uint32_t>(esp_timer_get_time() - t0);
  ++decodeCalls_;
  ++passCalls_;
  passSamples_ += static_cast<uint32_t>(got);
  if (us > maxDecodeUs_) maxDecodeUs_ = us;
  lastToc_ = toc;
  haveToc_ = true;
  made(out, static_cast<uint32_t>(got));
  return true;
}

bool OpusGenerator::loop() {
  if (!running) return false;
  const int64_t t0 = esp_timer_get_time();
  passSamples_ = passCalls_ = passMalformed_ = 0;
  passYield_ = false;
  pass_.begin(t0);
  bool more = true;
  for (;;) {
    if (outLeft_ > 0) {
      const uint16_t taken = output->ConsumeSamples(pcm_ + 2 * outAt_, static_cast<uint16_t>(outLeft_));
      outAt_ += taken;
      outLeft_ -= taken;
      if (outLeft_ > 0) break;  // refused (the ring full, the pass's budget spent): the rest next pass
    }
    // The pass's own caps (the class comment): what the output never saw
    // (frames dropped whole inside the pre-skip, malformed packets, pages
    // with no packet on them) can't make it refuse, so the pass ends here
    // instead; the next one goes on.
    if (passSamples_ >= kPassSamples || passCalls_ >= kPassCalls || passMalformed_ >= kPassMalformed || passYield_) break;
    // The pass's time (gate G6; PassClock): the pass's first step always,
    // then another only while this pass's time so far plus what the last
    // step took fits the budget. A step is a page slice's read, a decode
    // call or a fill, each a few ms, so the pass ends ~15 ms in unless one
    // step is longer than that by itself (a resync, an SD stall), and that
    // step ends only its own pass.
    const int64_t s0 = esp_timer_get_time();
    if (!pass_.fits(s0)) break;
    const bool stepped = decodeNext();
    pass_.stepped(s0, esp_timer_get_time());
    if (!stepped) {
      more = false;
      break;
    }
  }
  const auto us = static_cast<uint32_t>(esp_timer_get_time() - t0);
  if (us > maxLoopUs_) maxLoopUs_ = us;
  if (!more) {
    ended_ = true;
    running = false;
  }
  return more;
}

bool OpusGenerator::endedEarly() const {
  if (!reader_ || !ended_) return false;
  if (gapEnded_) return true;
  switch (reader_->ended()) {
    case oggopus::Reader::End::Truncated:
    case oggopus::Reader::End::Chained:
      return true;
    case oggopus::Reader::End::Eos:
      return !tl_.finished();  // the EOS page promised more than it held
    case oggopus::Reader::End::None:
    default:
      return false;  // the EOS trim ended it: the natural end
  }
}

const char* OpusGenerator::endText() const {
  if (!reader_) return "";
  if (!ended_) return "not at its end (stopped: the bench's 20 s, or a stop or skip)";
  if (gapEnded_) return "over 10 s lost to damaged pages: too much to fill, ended here";
  switch (reader_->ended()) {
    case oggopus::Reader::End::Truncated:
      return "the file ends before the stream's end (cut short, or damaged past here)";
    case oggopus::Reader::End::Chained:
      return "another stream follows in the file (chained): only the first plays";
    case oggopus::Reader::End::Eos:
      return tl_.finished() ? "the EOS page" : "the EOS page promised more samples than it held";
    case oggopus::Reader::End::None:
    default:
      return "the EOS trim";
  }
}

void OpusGenerator::logEnd(const char* what) const {
  if (!reader_) return;
  const oggopus::Reader::Stats& s = reader_->stats();
  const uint64_t kept = tl_.kept();
  const uint64_t length = reader_->lengthSamples();
  // What a whole play keeps: the length, less the start when the track
  // began part of the way in (a plan's target). Gate G9 reads "exactly the
  // length" from a play from the top; a seek's or a resume's play says
  // "from the start on" and the start's sample beside its landing.
  const uint64_t target = startTarget();
  const uint64_t expect = length > target ? length - target : 0;
  const char* from = target ? " from the start on" : "";
  char match[64];
  if (reader_->lastGranule() < 0) {
    snprintf(match, sizeof(match), "length unknown");
  } else if (kept == expect) {
    snprintf(match, sizeof(match), "exactly the length%s", from);
  } else {
    snprintf(match, sizeof(match), "%s the exact length%s by %llu", kept < expect ? "SHORT of" : "PAST", from,
             (unsigned long long)(kept < expect ? expect - kept : kept - expect));
  }
  char start[80] = "";
  if (planned_) {
    snprintf(start, sizeof(start), "; the start: sample %llu asked, %lld landed%s", (unsigned long long)target,
             (long long)landedT_, landedT_ < 0 ? " (nothing kept yet)" : landedT_ == static_cast<int64_t>(target) ? " (exact)" : " (OFF)");
  }
  Serial.printf("[opus] %s: %s; %lu packets, %lu decode calls (%lu concealed), %lu malformed (concealed), %lu dropped, "
                "%lu skipped, %lu gaps (%lu filled: %llu samples in all); kept %llu samples at 48 kHz, %s (%llu)%s; pages "
                "%lu, bad %lu, resyncs %lu (%lu B), sequence gaps %lu, dropped partials %lu, foreign %lu, oversized "
                "%lu; timeline corrections %lu (slip %lld); longest decode call %lu us, longest step %lu us, longest "
                "loop() %lu us; decode stack free since boot %lu B\n",
                what, endText(), (unsigned long)packets_, (unsigned long)decodeCalls_, (unsigned long)concealed_,
                (unsigned long)malformed_, (unsigned long)dropped_, (unsigned long)skipped_, (unsigned long)gaps_,
                (unsigned long)gapsFilled_, (unsigned long long)filled_, (unsigned long long)kept, match,
                (unsigned long long)length, start, (unsigned long)s.pages, (unsigned long)s.badPages, (unsigned long)s.resyncs,
                (unsigned long)s.resyncBytes, (unsigned long)s.sequenceGaps, (unsigned long)s.droppedPartials,
                (unsigned long)s.foreignPages, (unsigned long)s.oversized, (unsigned long)tl_.corrections(),
                (long long)tl_.slip(), (unsigned long)maxDecodeUs_, (unsigned long)pass_.maxStepUs(),
                (unsigned long)maxLoopUs_, (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
  char where[96];
  describeState(where, sizeof(where));
  Serial.printf("[opus] state: %s%s\n", where,
                highHeld_ ? " (blocks held below the line to get there: see the O knob)" : "");
}
