// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "audio/Core2AudioBackend.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorMP3.h>
#include <esp_cpu.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <new>

#include "ByteStream.h"
#include "RateConverter.h"
#include "ResamplerTables.h"
#include "ResumeAnchor.h"
#include "TableCopy.h"
#include "ToneTrack.h"
#include "TrackProgress.h"
#include "TrackSeek.h"
#include "app/Psram.h"
#include "audio/GuardedSource.h"
#include "audio/OpusGenerator.h"
#include "audio/PinnedMp3.h"
#include "audio/RingOutput.h"
#include "audio/SourceReader.h"
#include "storage/FileStream.h"

namespace {
constexpr uint32_t kRingFrames = 65536;  // ~1.5 s at 44.1 kHz (every track's rate in the ring), 256 KB of PSRAM
constexpr uint32_t kChunkFrames = 1024;  // source frames taken per pass of the decode task
// Internal RAM. MP3 and FLAC use ~3 KB of it; libopus keeps its scratch on
// the stack (VAR_ARRAYS). The M0 gate (docs/OPUS.md, G4) measured the most
// the task ever used at 13,000 B, the fuzz file's (random packets in every
// configuration) included, and 12,088 B on real files, so 16 KB, what
// 0.6.0 had, leaves 3,384 B on hostile input and 4,296 B on real files:
// the research's rule (the measured maximum + 3 KB, rounded up to 1 KB).
// M0's gate build ran at 20,480 to measure that; the 4 KB came back for
// the internal-RAM floor (G5).
constexpr uint32_t kDecodeStack = 16384;
constexpr uint32_t kRingRate = AudioShared::kRingRate;
// Bluetooth: what the headphones report plus ESP-IDF's frame queue and the
// air (an estimate); without a report, what the Powerbeats Pro report.
constexpr uint32_t kBtExtraUs = 25000;
constexpr uint32_t kBtDefaultReportUs = 150000;
// The speaker before its first buffer has been timed: ~3 buffers + DMA.
constexpr uint32_t kSpeakerDefaultUs = 115000;
constexpr uint32_t kBenchSeconds = 20;
// The converter's bench (Rb): seconds of audio per rate, and the rates.
constexpr uint32_t kRateBenchSeconds = 10;
constexpr uint32_t kRateBenchRates[] = {48000, 96000, 88200, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 44100};
// An MP3's first bytes after its tags (its Xing/VBRI header, PSRAM), and
// after a seek a frame and the next one's header (a frame is at most 1,441
// bytes).
constexpr uint32_t kMp3Probe = 4096;

// "1:23" (m:ss).
void mmss(uint32_t ms, char* buf, size_t size) {
  const uint32_t s = ms / 1000;
  snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// "1:23.456" (m:ss.mmm).
void mmssms(uint32_t ms, char* buf, size_t size) {
  const uint32_t s = ms / 1000;
  snprintf(buf, size, "%lu:%02lu.%03lu", (unsigned long)(s / 60), (unsigned long)(s % 60), (unsigned long)(ms % 1000));
}

// FNV-1a of a track's path: the run index's check that a run is this file's.
uint32_t pathHash(const std::string& path) {
  uint32_t h = 2166136261u;
  for (const char c : path) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h;
}

std::string extensionOf(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// Whose a resume anchor is, for the line that refuses it on another kind
// of file ("the resume anchor isn't this file's (a FLAC's): by its second").
const char* anchorKindName(ResumeAnchor::Kind k) {
  switch (k) {
    case ResumeAnchor::Kind::Mp3: return "an MP3's";
    case ResumeAnchor::Kind::Flac: return "a FLAC's";
    case ResumeAnchor::Kind::Opus: return "an Opus track's";
    case ResumeAnchor::Kind::None: break;
  }
  return "none";
}

// Swallows decoded audio and counts it, for bench(). Refuses a sample every
// kBurst frames so the generator's loop() returns and the time can be checked.
class CountingOutput : public AudioOutput {
public:
  bool begin() override { return true; }
  bool SetRate(int hz) override {
    rate = hz;
    return true;
  }
  bool SetChannels(int) override { return true; }
  bool ConsumeSample(int16_t[2]) override {
    if (budget == 0) return false;
    --budget;
    ++frames;
    return true;
  }
  bool stop() override { return true; }

  static constexpr uint32_t kBurst = 4096;
  int rate = 0;
  uint32_t budget = 0;
  uint64_t frames = 0;
};
}  // namespace

// libFLAC's seek (its SEEKTABLE when the file has one, else a bisection on
// the frames' headers), which AudioGeneratorFLAC doesn't expose: its decoder
// is a protected member.
class SeekableFlac : public AudioGeneratorFLAC {
public:
  // After begin(): to `sample`. The seek reads the metadata, then decodes
  // the frame the sample is in and hands it over from that sample (write_cb
  // keeps it for loop()). loop() learns the stream's format only after a
  // frame of its own, so it is set here: otherwise that first frame would
  // be read as 8-bit. (A rate the converter refuses fails the track at its
  // first loop(), as it would from the top.) False: not there; the decoder
  // is then in its seek error state: start again.
  bool seekTo(uint64_t sample) {
    if (!flac || !FLAC__stream_decoder_seek_absolute(flac, sample)) return false;
    sampleRate = FLAC__stream_decoder_get_sample_rate(flac);
    channels = static_cast<uint16_t>(FLAC__stream_decoder_get_channels(flac));
    bitsPerSample = static_cast<uint16_t>(FLAC__stream_decoder_get_bits_per_sample(flac));
    if (sampleRate == 0 || channels == 0) return false;
    output->SetRate(static_cast<int>(sampleRate));
    output->SetChannels(channels);
    return true;
  }
};

// The converter's polyphase tables copied out of flash (7.6 KB) while a
// track at another rate than 44.1 kHz plays: copied when one starts, freed
// when a 44.1 kHz one starts (TableCopy; RateConverter calls this from
// setRate(), after the reset, so no track reads a freed copy). Read from
// flash they share the cache with the decoder, and 147/160's 7 KB, read
// every 3.3 ms, evicts it: docs/RESAMPLER.md, section 10. Without the room
// they stay in flash (the same bits, slower), logged once. On the decode
// task.
//
// Where the copy goes (the console's Ot1 / Ot0; docs/OPUS.md gate G5 and
// section 8.11): by default a block of its own in the PSRAM's fast lower
// half, pinned at boot next to the decoder arena's (tablesBlock), so the
// 7,776 B of internal RAM stay free while it plays (every Opus track is
// 48 kHz and holds the copy, and M0's gate measured 44 KB of internal RAM
// free against the 50 KB floor); with Ot0, internal RAM, 0.5.0's and
// 0.6.0's place. The bits are the same; the cost is in the cache, which
// the PSRAM shares with the flash the decoder runs from (reading the
// tables from flash cost +13.5 points of a core that way, RESAMPLER.md
// section 10), so the PSRAM block was a knob, off, until the device said
// what it costs: M2's check measured the converter at 8.3-8.5 % of a core
// "in the decoder's company" against 5.6 % with the internal-RAM copy,
// +2.8 points (the copy stays cached), and internal RAM at 56 KB steady
// with an Opus track playing and the headphones linked against 48 KB, so
// the default flipped. The knob applies from the next copy: a copy in the
// other place is dropped at the next request's start (never inside a
// chain of joins, below).
static void* tablesBlock = nullptr;           // the pinned PSRAM block (begin()), or null
static std::atomic<bool> tablesPinned{true};  // the knob: the next copy goes to tablesBlock (Ot0: internal RAM)
static bool tablesCopyPinned = false;          // where the copy there is (decode task)
static void* tableAlloc(size_t bytes) {
  if (tablesPinned.load(std::memory_order_relaxed) && tablesBlock && bytes <= TableCopy::kBytes) {
    tablesCopyPinned = true;
    return tablesBlock;
  }
  tablesCopyPinned = false;
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tableFree(void* p) {
  if (p == tablesBlock) return;  // (pinned: kept for the next copy)
  heap_caps_free(p);
}
static TableCopy tableCopy(tableAlloc, tableFree);
// tableCopy's state for tableStatus() (written on the decode task only).
static std::atomic<bool> tablesInRam{false};
static std::atomic<bool> tablesInPsram{false};
static std::atomic<uint32_t> tableNoRoom{0};
// A chain of gapless joins is under way (decode task): the copy isn't
// freed until the next request's start, even at a join to a 44.1 kHz
// track. A cut rewinds the converter to a state saved earlier in the chain
// (RingFeed::mark()), whose rows may point into the copy: kept, every row
// ever saved stays valid (docs/GAPLESS.md section 5.1).
static bool tablesKeep = false;

static void tablesApply(bool wanted, const char* why) {
  const TableCopy::Event e = tableCopy.want(wanted);
  tablesInRam.store(tableCopy.copied() && !tablesCopyPinned, std::memory_order_relaxed);
  tablesInPsram.store(tableCopy.copied() && tablesCopyPinned, std::memory_order_relaxed);
  tableNoRoom.store(tableCopy.failures(), std::memory_order_relaxed);
  const unsigned bytes = (unsigned)TableCopy::kBytes;
  const unsigned freeNow = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  // The largest block too: the next copy needs one 7,776 B block, so a long
  // mixed session's fragmentation shows here before it bites.
  const unsigned largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  switch (e) {
    case TableCopy::Event::Copied:
      Serial.printf("[rate] the filter tables copied into %s (%u B): internal free %u B, largest block %u B\n",
                    tablesCopyPinned ? "the pinned PSRAM block (Ot1)" : "internal RAM", bytes, freeNow, largest);
      break;
    case TableCopy::Event::Freed:
      Serial.printf("[rate] %s: the filter tables' copy freed (%u B): internal free %u B, largest block %u B\n", why,
                    bytes, freeNow, largest);
      break;
    case TableCopy::Event::NoRoom:
      Serial.printf("[rate] no %u B block of internal RAM for the filter tables (largest %u B): read from flash, "
                    "the same bits, slower (logged once; each converted track tries again; R counts them)\n",
                    bytes, largest);
      break;
    case TableCopy::Event::StillNoRoom:
    case TableCopy::Event::None:
      break;
  }
}

static void tablesWanted(bool wanted) {
  if (!wanted && tablesKeep) return;
  tablesApply(wanted, "a 44.1 kHz track");
}

Core2AudioBackend::TableStatus Core2AudioBackend::tableStatus() {
  return {tablesInRam.load(std::memory_order_relaxed), tablesInPsram.load(std::memory_order_relaxed),
          tableNoRoom.load(std::memory_order_relaxed)};
}

void Core2AudioBackend::setOpusTablesPinned(bool pinned) { tablesPinned.store(pinned, std::memory_order_relaxed); }
bool Core2AudioBackend::opusTablesPinned() const { return tablesPinned.load(std::memory_order_relaxed); }
bool Core2AudioBackend::opusTablesBlock(char* buf, size_t size) const {
  if (!tablesBlock) {
    snprintf(buf, size, "none (no PSRAM block at boot)");
    return false;
  }
  snprintf(buf, size, "at %p, %s", tablesBlock, DecoderArena::whereName(DecoderArena::where(tablesBlock, TableCopy::kBytes)));
  return true;
}

Core2AudioBackend::Core2AudioBackend() : mp3Arena_(PinnedMp3::kArenaParts, 2) {}
Core2AudioBackend::~Core2AudioBackend() = default;

bool Core2AudioBackend::begin(fs::FS* fs, const char* stateDir, const char* btSinkName) {
  // The decoders' state first, while the PSRAM window's fast lower 2 MB is
  // free (docs/RESAMPLER.md section 10d): one block, laid out for libmad
  // (25 KB) or for the Opus decoder and its frame's PCM (38 KB; docs/OPUS.md),
  // one track at a time. Without it they decode on per-track mallocs.
  {
    size_t opusParts[2];
    OpusGenerator::layout(opusParts);
    opusLayout_ = mp3Arena_.addLayout(opusParts, 2);
  }
  mp3Arena_.attach(heap_caps_aligned_alloc(DecoderArena::kAlign, mp3Arena_.bytes(), MALLOC_CAP_SPIRAM));
  {
    char where[96];
    describeMp3State(where, sizeof(where));
    Serial.printf("[audio] MP3 decoder state: %u B %s (the block is %u B: the Opus decoder's %u B layout shares it)\n",
                  (unsigned)mp3Arena_.layoutBytes(0), where, (unsigned)mp3Arena_.bytes(),
                  (unsigned)(opusLayout_ >= 0 ? mp3Arena_.layoutBytes(static_cast<size_t>(opusLayout_)) : 0));
  }
  // The converter's tables' pinned block, right after the arena's (the
  // copy's home by default, tablesApply() above; the console's Ot0 puts
  // the copy in internal RAM instead): allocated now so it lands in the
  // fast lower half too.
  tablesBlock = heap_caps_aligned_alloc(DecoderArena::kAlign, TableCopy::kBytes, MALLOC_CAP_SPIRAM);
  {
    char where[96];
    opusTablesBlock(where, sizeof(where));
    Serial.printf("[audio] the filter tables' pinned PSRAM block (Ot1, the default; Ot0: internal RAM): %u B %s\n",
                  (unsigned)TableCopy::kBytes, where);
  }
  // The run index's two slots (docs/SEEK.md section 4.3; 2 x 24 KB). Without
  // them: no seeks back into a run, and a pause's anchor only for a FLAC.
  for (SeekIndex::Entry*& e : indexSlots_) {
    e = static_cast<SeekIndex::Entry*>(
        heap_caps_malloc(SeekIndex::kCapacity * sizeof(SeekIndex::Entry), MALLOC_CAP_SPIRAM));
  }
  index_.setStorage(indexSlots_[0], indexSlots_[1], SeekIndex::kCapacity);
  auto* ringBuffer = static_cast<int16_t*>(
      heap_caps_malloc(kRingFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  chunk_ = static_cast<int16_t*>(
      heap_caps_malloc(kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!ringBuffer || !chunk_) return false;
  ring_.reset(new PcmRing(ringBuffer, kRingFrames));

  fs_ = fs;
  out_.reset(new RingOutput(*ring_));  // under 4 KB: internal RAM (RingOutput.h)
  checkKernel("at boot");  // the converter's fast kernel, only if it gives the C kernel's bits
  RateConverter::setTablesWanted(tablesWanted);
  if (fs_) file_.reset(new AudioFileSourceFS(*fs_));
  guard_.reset(new GuardedSource());
  // The Opus open cache (docs/OPUS.md section 10): its entries in PSRAM,
  // loaded from the card now (the loop task, before any play).
  if (fs_ && stateDir && stateDir[0]) opusCachePath_ = std::string(stateDir) + "/opus.idx";
  if (!opusCache_.begin(OpusOpenCache::kDefaultEntries, psramAlloc, psramFree)) {
    Serial.println("[opus] no PSRAM for the open cache: every open reads the file");
  }
  loadOpusCache();
  // Gapless playback's PSRAM (docs/GAPLESS.md section 6): the trim's hold
  // (16 KB) and two marks of the feed (~2 KB each: a cut's way back). Without
  // them tracks end as before (no hold: no end trim; no marks: no joins).
  holdBuf_ = static_cast<int16_t*>(heap_caps_malloc(TrimFeed::kMaxHold * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  out_->trim().setHoldBuffer(holdBuf_, holdBuf_ ? TrimFeed::kMaxHold : 0);
  for (RingFeed::Mark*& m : marks_) {
    void* mem = heap_caps_malloc(sizeof(RingFeed::Mark), MALLOC_CAP_SPIRAM);
    m = mem ? new (mem) RingFeed::Mark : nullptr;
  }
  engine_.reset(new GaplessEngine(*ring_, feed(), out_->trim(), book_, *this));
  engine_->setMarks(marks_[0], marks_[1]);
  engine_->setEnabled(gapless_.load(std::memory_order_relaxed));

  bt_.begin(*ring_, shared_, btSinkName);
  speaker_.begin(*ring_, shared_);
  setOutput(Output::Speaker);
  setVolume(volume_);

  // Internal-RAM stack: flash reads (LittleFS) can't run on a PSRAM stack.
  xTaskCreatePinnedToCore(taskEntry, "decode", kDecodeStack, this, kDecodePriority, &task_, APP_CPU_NUM);
  return task_ != nullptr;
}

// ---- control side (loop task) ----

void Core2AudioBackend::request(const std::string& path, Kind kind, uint32_t startMs, uint32_t hintMs, uint32_t asHz,
                                const ResumeAnchor& anchor) {
  if (!task_) return;  // begin() failed
  {
    std::lock_guard<std::mutex> guard(lock_);
    request_ = {path, kind, pauses_.load(), startMs, hintMs, asHz, anchor};
  }
  requestMs_.store(millis(), std::memory_order_relaxed);
  sync_.post(kind == Kind::Play ? Phase::Pending : Phase::Idle);
  xTaskNotifyGive(task_);
}

bool Core2AudioBackend::play(const std::string& path, uint32_t durationHintMs, uint32_t startMs) {
  StartAt at;
  at.ms = startMs;
  at.hintMs = durationHintMs;
  return play(path, at);
}

bool Core2AudioBackend::play(const std::string& path, const StartAt& at) {
  // Un-paused by the decode task once the old track is discarded (start()), so
  // a paused ring never plays a burst of the previous track first.
  transportPlaying_ = true;
  request(path, Kind::Play, at.ms, at.hintMs, 0, at.anchor);
  return true;
}

size_t Core2AudioBackend::failureNote(char* buf, size_t size) const {
  if (size == 0) return 0;
  buf[0] = 0;
  if (!failed()) return 0;
  std::lock_guard<std::mutex> guard(lock_);
  snprintf(buf, size, "%s", failNote_.c_str());
  return std::strlen(buf);
}

bool Core2AudioBackend::resumeAnchor(ResumeAnchor* out) const {
  // Anchors are on the trimmed timeline: only with gapless trimming on.
  if (!ring_ || !gapless() || !gaplessTrim() || sync_.phase() == Phase::Pending) return false;
  // The heard track's ring frames played, as positionMs() counts them (held
  // at B while a join waits), and its run in the index: the request's now
  // (a request under way gives none).
  const uint32_t gen = sync_.generation();
  const GaplessJoin::Status st = book_.status();
  uint32_t r = ring_->readPos();
  if (st.boundary && st.b.gen == gen && static_cast<int32_t>(r - st.b.heardAt) > 0) r = st.b.heardAt;
  const int32_t frames = static_cast<int32_t>(r - st.heardStart);
  return index_.anchorAt(gen, frames > 0 ? static_cast<uint32_t>(frames) : 0, kRingRate, out);
}

void Core2AudioBackend::stop() {
  transportPlaying_ = false;
  request("", Kind::Stop);
}

void Core2AudioBackend::bench(const std::string& path) {
  transportPlaying_ = false;
  request(path, Kind::Bench);
}

void Core2AudioBackend::playAsRate(const std::string& path, uint32_t asHz) {
  transportPlaying_ = true;
  request(path, Kind::Play, 0, 0, asHz);
}

void Core2AudioBackend::rateBench() {
  transportPlaying_ = false;
  request("", Kind::RateBench);
}

Core2AudioBackend::RateStatus Core2AudioBackend::rateStatus() const {
  RateStatus r;
  r.rate = convRate_.load(std::memory_order_relaxed);
  r.route = convRoute_.load(std::memory_order_relaxed);
  r.num = convNum_.load(std::memory_order_relaxed);
  r.den = convDen_.load(std::memory_order_relaxed);
  r.taken = convTaken_.load(std::memory_order_relaxed);
  r.made = convMade_.load(std::memory_order_relaxed);
  r.clamped = convClamped_.load(std::memory_order_relaxed);
  return r;
}

void Core2AudioBackend::pause() {
  pauses_.fetch_add(1);  // before paused: see start()
  shared_.paused = true;
  transportPlaying_ = false;
}

void Core2AudioBackend::resume() {
  shared_.paused = false;
  transportPlaying_ = true;
}

bool Core2AudioBackend::isPlaying() const {
  const Phase p = sync_.phase();
  return !shared_.paused &&
         (p == Phase::Pending || p == Phase::Decoding || p == Phase::Draining);
}

uint32_t Core2AudioBackend::durationMs() const {
  // The heard track's: exact once its file has ended (GaplessJoin's frozen
  // length); before that it is the decoding track, read as before.
  uint32_t exact = 0;
  if (book_.frozenLength(&exact)) return exact;
  const uint32_t known = knownDurationMs_.load(std::memory_order_relaxed);
  if (known) return known;
  // The estimate is of what is left from where the decoder began (the
  // first audio's file position): a start part of the way in adds its time.
  const uint32_t left =
      progress::estimateDurationMs(producedFrames_.load(std::memory_order_relaxed), kRingRate,
                                   srcPos0_.load(std::memory_order_relaxed), srcPos_.load(std::memory_order_relaxed),
                                   srcSize_.load(std::memory_order_relaxed));
  return left ? startMs_.load(std::memory_order_relaxed) + left : 0;
}

uint32_t Core2AudioBackend::positionMs() const {
  // Ring frames are 44.1 kHz whatever the track's rate, and ring frame n
  // sits at exactly n / 44100 s of the source (the converter's delay is
  // compensated): exact from the first frame. The heard track's, from its
  // first frame in the ring; held at its exact end while a join after it
  // waits to be taken (takeAdvance()).
  if (!ring_) return 0;
  return book_.positionMs(sync_.generation(), ring_->readPos());
}

bool Core2AudioBackend::positionKnown() const { return sync_.phase() != Phase::Pending; }

bool Core2AudioBackend::finished() const { return sync_.phase() == Phase::Ended; }
bool Core2AudioBackend::failed() const { return sync_.phase() == Phase::Failed; }

IAudioBackend::RateRefusal Core2AudioBackend::rateRefusal() const {
  RateRefusal r;
  if (!failed()) return r;
  r.hz = refusedHz_.load(std::memory_order_relaxed);
  r.needsCpu = r.hz != 0 && refusedForCpu_.load(std::memory_order_relaxed);
  return r;
}

void Core2AudioBackend::setOutput(Output output) {
  output_ = output;
  ring_->setConsumer(output == Output::Bluetooth ? BtSink::kConsumerId
                                                 : SpeakerSink::kConsumerId);
  Serial.printf("[audio] output: %s\n", output == Output::Bluetooth ? "bluetooth" : "speaker");
}

void Core2AudioBackend::setVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (output_ == Output::Bluetooth) {
    bt_.setVolume(percent);
    return;
  }
  volume_ = percent;
  speaker_.setVolume(volume_);
}

void Core2AudioBackend::stepVolume(int delta) {
  if (output_ == Output::Bluetooth) {
    bt_.stepVolume(delta);
    return;
  }
  volume_ = static_cast<uint8_t>(std::min(100, std::max(0, volume_ + delta)));
  speaker_.setVolume(volume_);
}

void Core2AudioBackend::setSpeakerVolume(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  speaker_.setVolume(volume_);
}

uint8_t Core2AudioBackend::volume() const {
  return output_ == Output::Bluetooth ? bt_.volume() : volume_;
}

uint32_t Core2AudioBackend::outputLatencyUs(Output output, char* how, size_t howLen) const {
  if (output == Output::Bluetooth) {
    const uint32_t report = bt_.delayReportUs();
    const uint32_t used = report ? report : kBtDefaultReportUs;
    snprintf(how, howLen, "bt: %s %.1f + %lu ms", report ? "report" : "no report, assumed", used / 1000.0f,
             (unsigned long)(kBtExtraUs / 1000));
    return used + kBtExtraUs;
  }
  const uint32_t queue = speaker_.queueLatencyUs();
  if (queue == 0) {
    snprintf(how, howLen, "speaker: not measured yet, assumed");
    return kSpeakerDefaultUs;
  }
  snprintf(how, howLen, "speaker: queue %.1f + dma %.1f ms", queue / 1000.0f, speaker_.dmaLatencyUs() / 1000.0f);
  return queue + speaker_.dmaLatencyUs();
}

void Core2AudioBackend::loop(uint32_t nowMs) {
  // Every pass: the Bluetooth media stream runs while the player plays on it,
  // and is suspended 3 s after that stops.
  bt_.update(nowMs, output_ == Output::Bluetooth && transportPlaying_);
  // The speaker amp's switches (the pump makes them: AmpGate).
  const uint32_t switches = speaker_.ampSwitches();
  if (switches != loggedAmpSwitches_) {
    loggedAmpSwitches_ = switches;
    const bool on = SpeakerSink::ampOn();
    const AmpGate::Why why = speaker_.ampWhy();
    Serial.printf("[speaker] amp %s (%s)\n", on ? "on" : "off: I2S stopped, AXP192 GPIO2 low",
                  why == AmpGate::Why::Asked   ? "asked: Pa"
                  : why == AmpGate::Why::Quiet ? "quiet for 2 s"
                                               : "audio to play");
  }
  // How the last start filled the ring, once it's full (or 5 s on).
  // Signed: nowMs was read at the top of the main loop, before the console
  // or a touch made the request, so it can be a few ms before requestMs_
  // (unsigned, that wrapped and logged every start at once, all -1).
  const uint32_t seq = startSeq_.load(std::memory_order_acquire);
  if (seq != loggedStartSeq_ &&
      (fullMs_.load(std::memory_order_relaxed) >= 0 ||
       static_cast<int32_t>(nowMs - requestMs_.load(std::memory_order_relaxed)) > 5000)) {
    loggedStartSeq_ = seq;
    const StartTiming t = startTiming();
    const RefillPacer::Config pace = refillPacing();
    char paced[40] = "off";
    if (pace.enabled) {
      snprintf(paced, sizeof(paced), "%.1fx from %lu ms", pace.capX10 / 10.0f, (unsigned long)pace.gentleFromMs);
    }
    Serial.printf("[audio] refill: first audio in the ring %ld ms after the request, 500 ms buffered at %ld ms, "
                  "%lu ms (steady) at %ld ms, full at %ld ms (-1: not reached); pacing %s\n",
                  (long)t.firstAudioMs, (long)t.ring500Ms, (unsigned long)kSteadyMs, (long)t.steadyMs, (long)t.fullMs,
                  paced);
  }
  // The heard track's longest pass, read live, not with the once-a-second
  // figures below: the device check reads `s` within a second of a request
  // and wants the new track's (docs/OPUS.md 8.11).
  stats_.maxPassUs = heardMaxPassUs_.load(std::memory_order_relaxed);
  // The Opus open cache's save, once a second looked at: a few seconds
  // after its last change (the decode task's put), so a run of opens (a
  // boot's resume, a seek) saves once, from here (the loop task, as the
  // queue is saved) and never on the decode task's start path; ten
  // seconds after a write that failed.
  if (nowMs - opusCacheCheckMs_ >= 1000) {
    opusCacheCheckMs_ = nowMs;
    bool due = false;
    {
      std::lock_guard<std::mutex> guard(opusCacheLock_);
      due = opusCache_.dirty() && !opusCachePath_.empty() && static_cast<int32_t>(nowMs - opusCacheDueMs_) >= 0;
    }
    if (due) saveOpusCache();
  }
  if (nowMs - lastStatsMs_ < 1000) return;
  const uint32_t pulled = bt_.framesPulled();
  stats_.btFramesPerSec =
      static_cast<uint32_t>(static_cast<uint64_t>(pulled - lastBtFrames_) * 1000 / (nowMs - lastStatsMs_));
  lastBtFrames_ = pulled;
  lastStatsMs_ = nowMs;

  stats_.bufferedMs = bufferedMsNow();
  stats_.underruns = shared_.underruns;
  // Producing (decode and convert) against the audio produced, in ring frames.
  const uint64_t frames = producedFrames_.load();
  const uint64_t busyUs = busyUs_.load();
  stats_.decodeLoad = frames ? static_cast<float>(busyUs) / (static_cast<float>(frames) * 1e6f / kRingRate) : 0.0f;
  stats_.decodeStackFree = uxTaskGetStackHighWaterMark(task_);
}

uint32_t Core2AudioBackend::bufferedMsNow() const {
  if (!ring_) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(ring_->size()) * 1000 / kRingRate);
}

uint32_t Core2AudioBackend::ringCapacityMs() const {
  if (!ring_) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(ring_->capacity()) * 1000 / kRingRate);
}

// ---- sharing core 1 with the UI (see the header) ----

void Core2AudioBackend::setRefillPacing(bool enabled, uint32_t capX10) {
  // At least 1.5x (RefillPacer::kMinCapX10): near 1x the paced ring would
  // stop growing at 500 ms.
  paceCapX10_.store(capX10 < RefillPacer::kMinCapX10 ? RefillPacer::kMinCapX10 : capX10, std::memory_order_relaxed);
  paceEnabled_.store(enabled, std::memory_order_relaxed);
}

RefillPacer::Config Core2AudioBackend::refillPacing() const {
  RefillPacer::Config c;
  c.enabled = paceEnabled_.load(std::memory_order_relaxed);
  c.capX10 = paceCapX10_.load(std::memory_order_relaxed);
  return c;
}

Core2AudioBackend::StartTiming Core2AudioBackend::startTiming() const {
  StartTiming t;
  t.seq = startSeq_.load(std::memory_order_acquire);
  t.firstAudioMs = firstAudioMs_.load(std::memory_order_relaxed);
  t.ring500Ms = ring500Ms_.load(std::memory_order_relaxed);
  t.steadyMs = steadyMs_.load(std::memory_order_relaxed);
  t.fullMs = fullMs_.load(std::memory_order_relaxed);
  return t;
}

std::string Core2AudioBackend::description() const {
  std::lock_guard<std::mutex> guard(lock_);
  return heardOverride_ ? heardDescription_ : description_;
}

std::string Core2AudioBackend::note() const {
  std::lock_guard<std::mutex> guard(lock_);
  return note_;
}

void Core2AudioBackend::setText(std::string& field, const std::string& value) {
  std::lock_guard<std::mutex> guard(lock_);
  field = value;
}

// ---- gapless playback: the loop's side ----

void Core2AudioBackend::setNext(const Next& next) {
  book_.setOffer(sync_.generation(), next.after, next.token, next.path, next.hintMs);
  nextAtUs_.store(esp_timer_get_time(), std::memory_order_relaxed);
  if (task_) xTaskNotifyGive(task_);  // a cut at once, not after a 10 ms rest on a full ring
}

// A figure raised and never lowered, from either task: the heard track's
// longest pass has two writers (takeAdvance() on the loop's side,
// produceDecoded() on the decode task's), and a plain store from one could
// put back a smaller value over the larger the other had just written.
static void raiseTo(std::atomic<uint32_t>& figure, uint32_t value) {
  uint32_t cur = figure.load();
  while (cur < value && !figure.compare_exchange_weak(cur, value)) {
  }
}

bool Core2AudioBackend::takeAdvance(uint32_t* token) {
  const uint32_t readPos = ring_ ? ring_->readPos() : 0;
  if (!ring_ || !book_.takeAdvance(sync_.generation(), readPos, token)) return false;
  index_.advance();  // the joined track's run is the heard one (its anchors)
  {
    // The joined track is the heard one: its description (nothing decodes
    // after it until now), and why it ended early, if it did.
    std::lock_guard<std::mutex> guard(lock_);
    heardOverride_ = false;
    note_ = aheadNote_;
    aheadNote_.clear();
  }
  trackSeq_.fetch_add(1, std::memory_order_release);
  advances_.fetch_add(1, std::memory_order_relaxed);
  // The joined track's longest pass is the heard track's from here: what
  // it made decoding ahead (maxPassUs_ is its figure: the decoder is at
  // most one track ahead, the book's one boundary at a time) and its
  // passes from now on (produceDecoded()). The token first, then the
  // figure set (the track before's goes), then raised to the decoding
  // figure once more: a pass of the joined track that ends between the
  // load and the store here goes into the heard figure on the decode task
  // (it sees the token) and the store would put the smaller value back
  // over it; the raise after sees that pass in maxPassUs_ and restores it.
  // Both sides raise by compare-exchange, so neither lowers what the other
  // wrote: see produceDecoded().
  heardToken_.store(*token);
  heardMaxPassUs_.store(maxPassUs_.load());
  raiseTo(heardMaxPassUs_, maxPassUs_.load());
  Serial.printf("[gapless] heard: the joined track plays (taken %.1f ms after its first frame was read)\n",
                (readPos - book_.status().heardStart) * 1000.0f / kRingRate);
  return true;
}

void Core2AudioBackend::setGapless(bool on) {
  gapless_.store(on, std::memory_order_relaxed);
  if (engine_) engine_->setEnabled(on);
  if (task_) xTaskNotifyGive(task_);
}

bool Core2AudioBackend::durationKnown() const {
  uint32_t exact = 0;
  return book_.frozenLength(&exact) || knownDurationMs_.load(std::memory_order_relaxed) > 0;
}

void Core2AudioBackend::printGapless() const {
  const GaplessJoin::Status st = book_.status();
  const uint32_t r = ring_ ? ring_->readPos() : 0;
  Serial.printf("[gapless] %s; trimming by the LAME tag %s (G0/G1, Gt0/Gt1)\n", gapless() ? "on" : "off (v0.5.0's ends)",
                gaplessTrim() ? "on" : "off");
  if (!st.offer) {
    Serial.println("[gapless] word: none for this request yet");
  } else if (st.offerNow.token == 0) {
    Serial.printf("[gapless] word: nothing follows track %lu\n", (unsigned long)st.offerNow.after);
  } else {
    Serial.printf("[gapless] word: track %lu is followed by %lu, %s%s\n", (unsigned long)st.offerNow.after,
                  (unsigned long)st.offerNow.token, st.offerNow.path.c_str(),
                  st.offerNow.token == st.lastTaken ? " (taken)" : "");
  }
  if (st.boundary) {
    const char* state = st.state == GaplessJoin::State::Pending   ? "cuttable"
                        : st.state == GaplessJoin::State::Cutting ? "being cut"
                                                                  : "too late to cut";
    Serial.printf("[gapless] boundary: track %lu at ring frame %lu (J %lu, %+.0f ms from the reader), %s\n",
                  (unsigned long)st.b.token, (unsigned long)st.b.heardAt, (unsigned long)st.b.cutAt,
                  static_cast<int32_t>(st.b.heardAt - r) * 1000.0f / kRingRate, state);
  } else {
    Serial.println("[gapless] boundary: none");
  }
  if (st.frozen) Serial.printf("[gapless] the heard track's file has ended: exactly %lu ms\n", (unsigned long)st.frozenMs);
  if (engine_) {
    const GaplessEngine::Counters& c = engine_->counters();  // (the decode task's: a snapshot)
    Serial.printf("[gapless] since boot: joins %lu continuous, %lu after the tail (%lu late); heard %lu; cuts %lu, too "
                  "late %lu, retried %lu; opens failed %lu, empty %lu; last cut %lu us after its word (max %lu)\n",
                  (unsigned long)c.joins, (unsigned long)c.resets, (unsigned long)c.late,
                  (unsigned long)advances_.load(std::memory_order_relaxed), (unsigned long)c.cuts,
                  (unsigned long)c.tooLate, (unsigned long)c.retries, (unsigned long)c.failedOpens,
                  (unsigned long)c.emptyAhead, (unsigned long)cutLatencyUs_.load(std::memory_order_relaxed),
                  (unsigned long)cutLatencyMaxUs_.load(std::memory_order_relaxed));
  }
  const uint32_t trim = trimNow_.load(std::memory_order_relaxed);
  if (trim & kTrimLame) {
    Serial.printf("[gapless] decoding track's trim: LAME delay %lu, padding %lu: skipping %lu, holding %lu%s\n",
                  (unsigned long)trimDelay_.load(std::memory_order_relaxed),
                  (unsigned long)trimPadding_.load(std::memory_order_relaxed),
                  (unsigned long)trimSkip_.load(std::memory_order_relaxed),
                  (unsigned long)trimHold_.load(std::memory_order_relaxed),
                  (trim & kTrimCrcBad) ? " (the tag's CRC doesn't match: trimmed anyway)" : "");
  } else if (trim & kTrimMp3) {
    Serial.printf("[gapless] decoding track's trim: no LAME tag: not trimmed (skipping the decoder's lead, %lu)\n",
                  (unsigned long)trimSkip_.load(std::memory_order_relaxed));
  }
  // The run index (docs/SEEK.md section 4.3): the heard track's run.
  SeekIndex::Run run;
  if (index_.heardRun(&run)) {
    Serial.printf("[gapless] run index: heard %s run from sample %llu (%s), %lu entries; decoding slot %lu entries\n",
                  run.kind == SeekIndex::Kind::Flac   ? "FLAC"
                  : run.kind == SeekIndex::Kind::Opus ? "Opus"
                                                      : "MP3",
                  (unsigned long long)run.base,
                  run.exact ? "exact" : "a TOC start's time", (unsigned long)index_.entries(index_.heardSlot()),
                  (unsigned long)index_.entries(index_.decodingSlot()));
  } else {
    Serial.println("[gapless] run index: no heard run (a built-in track, G0 or Gt0, or nothing played)");
  }
}

// ---- decode task ----

void Core2AudioBackend::taskEntry(void* self) { static_cast<Core2AudioBackend*>(self)->decodeTask(); }

void Core2AudioBackend::rest(uint32_t ms) {
  // Not vTaskDelay(): play()/stop()/setNext() wake us (xTaskNotifyGive),
  // so a cut or a new request never waits out a rest.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

void Core2AudioBackend::decodeTask() {
  uint32_t generation = sync_.generation();
  for (;;) {
    const uint32_t latest = sync_.generation();
    if (latest != generation) {
      generation = latest;
      start(generation);
      continue;
    }
    GaplessEngine& e = *engine_;
    const GaplessEngine::Phase p = e.phase();
    if (p == GaplessEngine::Phase::Idle || p == GaplessEngine::Phase::Ended) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // play()/stop() wake us early
      continue;
    }
    if (e.cutDue() || p != GaplessEngine::Phase::Producing) {
      // A source's end, the word on what follows, a join, the tail, the
      // drain, a cut: one step at a time, the generation checked between.
      const uint32_t ms = e.step(bufferedMsNow());
      // The step may have pushed the converter's tail (the flush's
      // finish()): the console's R counts it too, so after a track's end
      // "ring frames made" is ceil(taken x 147/160) with nothing left in
      // the filter (gate G9 reads that).
      publishRate();
      reportPhase(generation);
      if (ms) rest(ms);
      continue;
    }
    switch (toneTrack_ ? produceTone() : produceDecoded()) {
      case Produced::More:
        break;
      case Produced::Done:
        e.sourceEnded(early_);
        break;
      case Produced::Failed:
        failRate(generation);
        break;
    }
  }
}

void Core2AudioBackend::reportPhase(uint32_t generation) {
  Phase want = Phase::Decoding;
  switch (engine_->phase()) {
    case GaplessEngine::Phase::Draining:
      want = Phase::Draining;
      break;
    case GaplessEngine::Phase::Ended:
      want = Phase::Ended;
      break;
    case GaplessEngine::Phase::Idle:
      return;
    default:
      break;
  }
  if (want == reported_) return;
  if (want == Phase::Draining) {
    ringSteady_ = false;  // the ring empties on purpose now
    shared_.expectingAudio = false;
  } else if (want == Phase::Decoding) {
    shared_.expectingAudio = true;  // a late join (or a cut back from a drain): audio from here on
  }
  reported_ = want;
  sync_.report(generation, want);
}

void Core2AudioBackend::start(uint32_t generation) {
  Request req;
  {
    std::lock_guard<std::mutex> guard(lock_);
    req = request_;
  }
  closeDecoder();
  toneTrack_ = false;
  clickTrack_ = false;
  early_ = false;
  toneN_ = toneAt_ = 0;
  shared_.expectingAudio = false;
  ringSteady_ = false;  // filling from empty until kSteadyMs
  refusedHz_ = 0;
  out_->trim().disarm();
  // Nothing of the previous track plays after this, nor of one decoded
  // ahead of it (its boundary goes with the book's restart below)...
  const uint32_t at = ring_->discardAll();
  // ...nor of its converter's history (up to 24 frames), whatever comes
  // next: a file, a built-in track, a stop or a bench. A request is where
  // the tables' copy may go (a 44.1 kHz track); never during a chain of
  // joins (tablesKeep: a cut's rewind may need its rows). The Ot knob
  // moved since the copy was made: dropped here, so the next converted
  // track copies afresh where the knob says.
  tablesKeep = false;
  if (tableCopy.copied() && tablesCopyPinned != tablesPinned.load(std::memory_order_relaxed)) {
    tablesApply(false, "the Ot knob moved");
  }
  feed().reset(cpuMhz());
  publishRate();
  // The longest pass: the request's track is the one decoded and the one
  // heard from here (token 0: GaplessJoin), so both figures start over.
  // beginPrepared() starts the decoding track's over at a join's begin as
  // well; the heard track's then waits for the join to be heard
  // (takeAdvance()).
  maxPassUs_ = 0;
  heardToken_ = 0;
  heardMaxPassUs_ = 0;
  if (req.kind == Kind::Play) {  // a new start to time (startTiming())
    firstAudioMs_ = -1;
    ring500Ms_ = -1;
    steadyMs_ = -1;
    fullMs_ = -1;
    startSeq_.fetch_add(1, std::memory_order_release);
  }
  busyUs_ = 0;
  producedFrames_ = 0;
  srcPos0_ = srcPos_ = srcSize_ = 0;
  knownDurationMs_ = 0;
  // Where it was asked to start, so Now Playing shows that second from the
  // request on; corrected below once the track says where it really lands.
  startMs_ = req.kind == Kind::Play ? req.startMs : 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    description_.clear();
    heardOverride_ = false;
    aheadNote_.clear();
  }
  trimNow_ = 0;
  reported_ = Phase::Idle;
  if (req.kind != Kind::Play) {
    engine_->idle(generation);
    if (req.kind == Kind::Bench || req.kind == Kind::RateBench) {
      if (req.kind == Kind::Bench) runBench(req.path);
      if (req.kind == Kind::RateBench) runRateBench();
      feed().reset(cpuMhz());
      publishRate();
    }
    return;
  }
  // A test play at a forced rate (Rf) is never joined: the next track would
  // play at that rate too.
  engine_->begin(generation, at, req.startMs, req.asHz == 0);
  trackSeq_.fetch_add(1, std::memory_order_release);  // (after the book's restart: the new position with it)
  // A newly started track plays, unless the player paused since asking. A
  // pause() racing with this counts first and sets paused itself, so checking
  // again after un-pausing leaves it paused whichever store lands last.
  if (pauses_.load() == req.pauses) {
    shared_.paused = false;
    if (pauses_.load() != req.pauses) shared_.paused = true;
  }
  setText(note_, "");
  feed().forceRate(static_cast<int>(req.asHz));  // (a test: 0 almost always)

  Prepared& p = prepared_;
  StartAt startAt;
  startAt.ms = req.startMs;
  startAt.hintMs = req.hintMs;
  startAt.anchor = req.anchor;
  runGen_ = generation;
  const bool prepared = prepare(req.path, startAt, &p);  // (a seek back into a run looks it up first)
  index_.reset();                                    // then the request's track records afresh
  if (!prepared) {
    // With the reason when the file gave one (an Opus file refused: "Ogg
    // Vorbis isn't supported (only Opus)" in the log and the track's note,
    // and its few words, "Ogg Vorbis isn't supported", on Now Playing's
    // toast through failureNote()).
    fail(generation,
         !prepareWhy_.empty() ? prepareWhy_
                              : (req.path.rfind("tone:", 0) == 0 ? "unknown tone " : "can't play ") + req.path,
         prepareNote_.c_str());
    return;
  }
  if (!beginPrepared(p, out_.get(), true)) {
    if (feed().rejected()) {
      failRate(generation);
    } else {
      fail(generation, "can't play " + req.path);
    }
    return;
  }
  engine_->setStartMs(startMs_.load(std::memory_order_relaxed));
  reported_ = Phase::Decoding;
  sync_.report(generation, Phase::Decoding);
}

RingFeed& Core2AudioBackend::feed() { return out_->feed(); }

uint32_t Core2AudioBackend::cpuMhz() const {
  const uint16_t set = cpuMhz_.load(std::memory_order_relaxed);
  return set ? set : getCpuFrequencyMhz();
}

std::string Core2AudioBackend::refusalText() {
  return std::to_string(feed().rate()) + " Hz " + feed().refusal();
}

void Core2AudioBackend::publishRate() {
  const RingFeed& f = feed();
  const RateConverter& c = f.converter();
  const RateConverter::Plan& p = c.currentPlan();
  convRate_.store(static_cast<uint32_t>(f.rate()), std::memory_order_relaxed);
  convRoute_.store(c.configured() ? p.route : "", std::memory_order_relaxed);
  convNum_.store(p.num, std::memory_order_relaxed);
  convDen_.store(p.den, std::memory_order_relaxed);
  convTaken_.store(static_cast<uint32_t>(c.taken()), std::memory_order_relaxed);
  convMade_.store(static_cast<uint32_t>(c.produced()), std::memory_order_relaxed);
  convClamped_.store(c.clamped(), std::memory_order_relaxed);
}

void Core2AudioBackend::failRate(uint32_t generation) {
  // Stored before fail()'s report, so whoever sees the failure sees why.
  refusedForCpu_.store(feed().refusalKind() == RateConverter::Refusal::NeedsCpu, std::memory_order_relaxed);
  refusedHz_.store(static_cast<uint32_t>(feed().rate()), std::memory_order_relaxed);
  // The console's R reports the refused rate (a built-in track at another
  // rate is refused before any pass publishes it).
  publishRate();
  fail(generation, refusalText());
}

void Core2AudioBackend::fail(uint32_t generation, const std::string& why, const char* note) {
  Serial.printf("[audio] %s\n", why.c_str());
  closeDecoder();
  setText(note_, why);
  setText(failNote_, note);  // (before the Failed report: whoever sees the failure sees it)
  engine_->idle(generation);
  reported_ = Phase::Failed;
  sync_.report(generation, Phase::Failed);
}

void Core2AudioBackend::endedEarly(const std::string& why) {
  early_ = true;
  Serial.printf("[gapless] %s ended early: %s\n", prepared_.path.c_str(), why.c_str());
  // Its note shows once it is the heard track.
  const bool ahead = book_.status().boundary;
  setText(ahead ? aheadNote_ : note_, why);
}

bool Core2AudioBackend::prepare(const std::string& path, const StartAt& at, Prepared* p) {
  *p = Prepared{};
  p->path = path;
  prepareWhy_.clear();
  prepareNote_.clear();
  const uint32_t startMs = at.ms;
  const bool gapless = gapless_.load(std::memory_order_relaxed);
  const bool anchorsOn = gapless && gaplessTrim_.load(std::memory_order_relaxed);
  if (path.rfind("tone:", 0) == 0) {
    // A built-in track (lib/core ToneTrack): made at its own rate and
    // converted like a file, so the test tracks at other rates go through
    // the converter (and its refusals) as a file would. Sample-exact: no
    // trim. Part of the way in it counts from there: the same sound, only
    // what is left of its length (a click track's grid starts again at the
    // start, so the dancer's truth, in track frames from the start, holds).
    if (!ToneTrack::parse(path, &p->toneSpec)) return false;
    p->tone = true;
    p->rate = p->toneSpec.rate;
    p->knownMs = p->toneSpec.seconds * 1000;
    p->landedMs = trackseek::startMs(startMs, p->knownMs);
    if (startMs > 0) {
      char asked[12];
      mmss(startMs, asked, sizeof(asked));
      Serial.printf("[audio] built-in track: %s asked: %s\n", asked, p->landedMs ? "counting from there" : "from 0:00");
    }
    return true;
  }
  const std::string ext = extensionOf(path);
  p->mp3 = ext == ".mp3";
  p->opus = ext == ".opus";
  if (!fs_ || (!p->mp3 && !p->opus && ext != ".flac")) return false;
  if (!file_->open(path.c_str())) return false;
  const uint32_t size = file_->getSize();
  p->pathHash = pathHash(path);
  p->fileSize = size;

  if (p->opus) {
    // Ogg Opus (docs/OPUS.md): the generator reads the headers and the
    // last page now (~10 reads): its rate is always 48 kHz (a join's
    // continuity is decided before any frame), its length is exact from
    // the open, and a file it can't play fails here with the reason as the
    // note. The generator does its own trimming (the pre-skip, the EOS
    // trim): p->trim stays {0,0}. A start part of the way in is the
    // reader's plan (planOpus()), begun by the generator (beginPrepared()).
    OpusGenerator* g = opusGenerator();
    if (!g) {
      prepareWhy_ = prepareNote_ = "no RAM for the Opus decoder";
      return false;
    }
    g->setPlacement(static_cast<OpusGenerator::Placement>(opusPlacement_.load(std::memory_order_relaxed)));
    // The open cache's record for this path at this size (docs/OPUS.md
    // section 10): the generator opens from it with one read when the
    // file is still the one it describes; a fresh open's record goes in
    // (replacing a stale one), and a file whose length can't be known
    // (no last page found) leaves none.
    oggopus::OpenRecord hint;
    const bool haveHint = findOpusRecord(path, size, &hint);
    if (!g->open(file_.get(), output_ == Output::Speaker, haveHint ? &hint : nullptr)) {
      Serial.printf("[audio] %s: %s\n", path.c_str(), g->refusal());
      prepareWhy_ = g->refusal();
      prepareNote_ = g->refusalNote();
      if (haveHint) forgetOpusRecord(path, size);
      return false;
    }
    if (!g->openedFromRecord()) {
      oggopus::OpenRecord rec;
      if (g->record(&rec)) {
        putOpusRecord(path, rec);
      } else if (haveHint) {
        forgetOpusRecord(path, size);
      }
    }
    p->rate = oggopus::kRate;
    p->knownMs = g->lengthMs();
    p->totalSamples = g->lengthSamples();  // (the run's header: a pause's anchor carries it; 0: not known)
    if (startMs > 0 || at.anchor.valid()) planOpus(g, at, p);
    return true;
  }

  if (p->mp3) {
    // Past the ID3v2 tags (one after another, as some taggers leave them)
    // to the first frame: no ID3 reader (the library has the title and the
    // artist; ESP8266Audio's reads a tag a byte at a time, above the UI,
    // and at a join that would be while the track before plays).
    uint32_t start = 0;
    for (int i = 0; i < 4; ++i) {
      uint8_t head[10];
      if (!file_->seek(static_cast<int32_t>(start), SEEK_SET) || file_->read(head, sizeof(head)) != sizeof(head)) break;
      const uint32_t tag = progress::id3v2Size(head, sizeof(head));
      if (tag == 0 || start + tag >= size) break;
      start += tag;
    }
    if (start > 16 * 1024) {
      Serial.printf("[audio] ID3 tag of %lu KB (a picture?): skipped, not read\n", (unsigned long)(start / 1024));
    }
    p->from = start;
    // The first frame (PSRAM): its Xing/Info or VBRI header (the length,
    // trimmed by LAME's tag and scaled down for a file shorter than its
    // header says; a frame with no audio, not decoded), its rate (a join's
    // continuity is decided before any frame), and a start part of the way
    // in (planMp3()).
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(kMp3Probe, MALLOC_CAP_SPIRAM));
    if (buf) {
      if (start < size && file_->seek(static_cast<int32_t>(start), SEEK_SET)) {
        const uint32_t got = file_->read(buf, kMp3Probe);
        lametag::parse(buf, got, &p->lame);
        p->knownMs = progress::mp3HeaderDurationMs(buf, got, size, start);
        p->rate = p->lame.frame ? p->lame.rate : 0;
        // From the top the decoder is handed the first audio frame (G1:
        // never the header frame, nor junk before the first frame).
        p->firstAudio = trackseek::firstAudioByte(buf, got, start);
        if (gapless && p->lame.frame) p->from = p->firstAudio;
        if (p->lame.frame && p->firstAudio >= start && p->firstAudio - start < got) {
          p->firstHash = resumeanchor::frameHash(buf + (p->firstAudio - start), got - (p->firstAudio - start));
        }
        const bool useTag = anchorsOn && p->lame.lame;
        p->topT0 = useTag ? -static_cast<int32_t>(p->lame.delay + lametag::kDecoderDelay) : 0;
        if (startMs > 0 || at.anchor.valid()) {
          auto* scratch = static_cast<uint8_t*>(heap_caps_malloc(trackseek::kScratchBytes, MALLOC_CAP_SPIRAM));
          if (scratch) {
            planMp3(buf, got, start, at, scratch, p);
            heap_caps_free(scratch);
          } else {
            Serial.println("[audio] MP3: no PSRAM to plan the start: from 0:00");
          }
        }
      }
      heap_caps_free(buf);
    }
    // The trim: the generator's lead always (its constructor's {0,0}, not
    // in the file), LAME's delay + 529 from the top and its padding - 529
    // at the end (Gt0: neither). G0: nothing, as v0.5.0. A planned start's
    // lead and preroll are the landing phase's (TrimFeed::armAt()).
    if (gapless) p->trim = lametag::trim(p->lame, 1, p->fromTop, gaplessTrim_.load(std::memory_order_relaxed));
    p->guard = gapless;
    return true;
  }

  // FLAC: its length and rate from STREAMINFO (the decoder doesn't expose
  // them before its first frame). Some taggers put an ID3v2 tag in front of
  // "fLaC": libFLAC skips it, and so does this.
  uint8_t head[42];
  uint32_t tag = 0;
  if (file_->read(head, sizeof(head)) == sizeof(head)) {
    tag = progress::id3v2Size(head, sizeof(head));
    if (tag > 0 && !(tag < size && file_->seek(static_cast<int32_t>(tag), SEEK_SET) &&
                     file_->read(head, sizeof(head)) == sizeof(head))) {
      std::memset(head, 0, sizeof(head));
    }
    p->knownMs = progress::flacDurationMs(head, sizeof(head));
    if (!trackseek::flacStreamInfo(head, sizeof(head), &p->rate, &p->totalSamples)) p->rate = 0;
  }
  // Its metadata blocks: libFLAC reads through the ones it doesn't keep (an
  // embedded picture: megabytes) before the first frame, so at a join a big
  // one must fit in what the ring holds (logged; docs/GAPLESS.md section 12).
  if (p->rate) {
    uint32_t at = tag + 4;
    for (int i = 0; i < 64 && at + 4 <= size; ++i) {
      uint8_t h[4];
      if (!file_->seek(static_cast<int32_t>(at), SEEK_SET) || file_->read(h, sizeof(h)) != sizeof(h)) break;
      const uint32_t len = (static_cast<uint32_t>(h[1]) << 16) | (static_cast<uint32_t>(h[2]) << 8) | h[3];
      p->metadataBytes += 4 + len;
      at += 4 + len;
      if (h[0] & 0x80) break;  // the last block
    }
  }
  if (startMs > 0 || at.anchor.valid()) {
    // libFLAC's own seek, by sample (exact), after begin(): the resume
    // anchor's sample when it is this file's (its size, rate and total
    // samples), else the millisecond's.
    bool byAnchor = false;
    const ResumeAnchor& a = at.anchor;
    if (a.valid()) {
      const char* why = !anchorsOn                              ? "gapless trimming off"
                        : a.kind != ResumeAnchor::Kind::Flac    ? anchorKindName(a.kind)
                        : a.fileSize != size                    ? "the size"
                        : p->rate == 0 || a.rate != p->rate     ? "the rate"
                        : a.frameHash != static_cast<uint32_t>(p->totalSamples) ? "the length"
                        : resumeanchor::ms(a) > 0 && trackseek::startMs(resumeanchor::ms(a), p->knownMs) == 0
                            ? "in its last 5 s"
                            : nullptr;
      if (why) {
        Serial.printf("[audio] FLAC: the resume anchor isn't this file's (%s): by its second\n", why);
      } else {
        byAnchor = true;
        p->flacSeek = a.sample > 0;
        p->flacSample = a.sample;
      }
    }
    char asked[12];
    mmss(startMs, asked, sizeof(asked));
    const uint32_t landed = trackseek::startMs(startMs, p->knownMs);
    if (byAnchor) {
      // (sample 0: from the top, as asked)
    } else if (landed == 0) {
      Serial.printf("[audio] FLAC: %s asked: in its last %lu s or past its end: from 0:00\n", asked,
                    (unsigned long)(trackseek::kTailMs / 1000));
    } else if (p->rate == 0) {
      Serial.printf("[audio] FLAC: %s asked: no STREAMINFO found to go by: from 0:00\n", asked);
    } else {
      p->flacSeek = true;
      p->flacSample = static_cast<uint64_t>(landed) * p->rate / 1000;
    }
    if (p->flacSeek) {
      // positionMs() and the resume point count from there.
      p->landedMs = static_cast<uint32_t>(p->flacSample * 1000 / p->rate);
      p->startSample = p->flacSample;
      p->fromTop = false;
    }
  }
  // The trim: after a seek the generator's lead ({0,0}, once it knows its
  // channels); from the top there is none. FLAC is sample-exact.
  if (gapless && !p->fromTop) p->trim.skip = 1;
  return true;
}

bool Core2AudioBackend::beginPrepared(const Prepared& p, AudioOutput* out, bool trimmed) {
  recorder_.stop();
  if (trimmed) {
    // (A planned start is armed once its generator, the cursor, exists.)
    out_->trim().arm(p.trim.skip, p.trim.hold);
  } else {
    out_->trim().disarm();
  }
  busyUs_ = 0;
  maxPassUs_ = 0;  // the decoding track's (at a join's begin the heard track's figure stays until the join is heard)
  producedFrames_ = 0;
  srcPos0_ = srcPos_ = srcSize_ = 0;
  described_ = false;
  early_ = false;
  toneTrack_ = false;
  clickTrack_ = false;
  toneN_ = toneAt_ = 0;
  knownDurationMs_.store(p.knownMs, std::memory_order_relaxed);
  startMs_.store(p.landedMs, std::memory_order_relaxed);
  if (trimmed) {
    uint32_t trim = 0;
    if (p.mp3) trim |= kTrimMp3;
    if (p.mp3 && p.lame.lame && p.trim.hold + p.trim.skip > 1) trim |= kTrimLame;
    if (p.lame.crcChecked && !p.lame.crcOk) trim |= kTrimCrcBad;
    trimDelay_.store(p.lame.delay, std::memory_order_relaxed);
    trimPadding_.store(p.lame.padding, std::memory_order_relaxed);
    trimSkip_.store(p.trim.skip, std::memory_order_relaxed);
    trimHold_.store(p.trim.hold, std::memory_order_relaxed);
    trimNow_.store(trim, std::memory_order_relaxed);
  }

  if (p.tone) {
    const ToneTrack& t = p.toneSpec;
    if (!feed().setRate(static_cast<int>(t.rate))) return false;
    publishRate();
    const uint32_t frames =
        t.rate * t.seconds - static_cast<uint32_t>(static_cast<uint64_t>(p.landedMs) * t.rate / 1000);
    toneTrack_ = true;
    char text[64];
    switch (t.kind) {
      case ToneTrack::Kind::Clicks:
        click_.start(t.rate, t.click, frames);
        clickTrack_ = true;
        snprintf(text, sizeof(text), "clicks %.0f BPM%s, %lu Hz", t.click.bpm,
                 t.click.offsetBeats > 0 ? ", off-beat start" : "", (unsigned long)t.rate);
        break;
      case ToneTrack::Kind::Silence:
        tone_.startSilence(t.rate, frames);
        snprintf(text, sizeof(text), "silence (zeros, for power tests), %lu Hz", (unsigned long)t.rate);
        break;
      case ToneTrack::Kind::Sine:
      default:
        tone_.start(t.rate, t.hz, -18.0f, t.leftOnly ? ToneGen::Channels::LeftOnly : ToneGen::Channels::Both, frames);
        snprintf(text, sizeof(text), "tone %.0f Hz%s, %lu Hz", t.hz, t.leftOnly ? ", left only" : "",
                 (unsigned long)t.rate);
        break;
    }
    setText(description_, text);
    if (t.rate != kRingRate) {
      Serial.printf("[audio] %s: %lu Hz -> %lu Hz (%s)\n", p.path.c_str(), (unsigned long)t.rate,
                    (unsigned long)kRingRate, feed().converter().currentPlan().route);
    }
    return true;
  }

  // A fresh generator per track: both keep state across begin() (FLAC its
  // sample buffer, which can replay freed memory; MP3 its last sample). Their
  // big buffers are allocated in begin() anyway, so this costs little.
  AudioGenerator* decoder;
  AudioFileSource* source = file_.get();
  if (p.mp3) {
    if (!file_->seek(static_cast<int32_t>(p.from), SEEK_SET)) {
      closeDecoder();
      return false;
    }
    if (p.guard) {
      guard_->attach(file_.get());
      source = guard_.get();
    }
    mp3_.reset(makeMp3());
    if (!mp3_) {
      closeDecoder();
      return false;
    }
    decoder = mp3_.get();
    if (trimmed && p.planned) {
      // The landing phase: the lead and the preroll dropped until the cursor
      // says the landing frame, then the plan's skip; the end held as any
      // start's.
      const trackseek::Plan& pl = p.plan;
      out_->trim().armAt(mp3_.get(), pl.landByte, pl.landLength, pl.spf, pl.skip, p.trim.hold);
    }
    if (trimmed && gapless_.load(std::memory_order_relaxed)) {
      if (p.lame.lame && gaplessTrim_.load(std::memory_order_relaxed)) {
        Serial.printf("[gapless] trim: %s delay %u, padding %u: skipping %lu, holding %lu%s\n", p.lame.encoder,
                      (unsigned)p.lame.delay, (unsigned)p.lame.padding, (unsigned long)p.trim.skip,
                      (unsigned long)p.trim.hold, p.lame.crcChecked && !p.lame.crcOk ? " (tag CRC mismatch)" : "");
      } else if (!p.lame.lame) {
        Serial.printf("[gapless] %s: no LAME tag: not trimmed\n", p.lame.header ? "a header frame (skipped)" : "no header");
      }
    }
  } else if (p.opus) {
    // Opened by prepare() (the headers, the length, the plan of a start
    // part of the way in); begin() puts the decoder on its state and the
    // reader at the first audio packet, or at the plan's page.
    decoder = opus_.get();
    if (!decoder) {
      closeDecoder();
      return false;
    }
    opus_->setStartPlan(p.opusPlanned ? &p.opusPlan : nullptr);
  } else {
    file_->seek(0, SEEK_SET);
    flac_.reset(new SeekableFlac());
    decoder = flac_.get();
  }
  if (!decoder->begin(source, out)) {
    closeDecoder();
    return false;
  }
  decoder_ = decoder;
  codec_ = p.mp3 ? "MP3" : p.opus ? "Opus" : "FLAC";
  if (p.opus) {
    char where[96];
    opus_->describeState(where, sizeof(where));
    Serial.printf("[opus] decoding: %s, %lu ms long; the state %s\n", p.path.c_str(), (unsigned long)p.knownMs, where);
  }
  // The run index: an MP3's entries, or a FLAC's or an Opus track's header
  // alone (a pause's anchor comes from it: resumeAnchor()).
  if (trimmed) beginRun(p);
  if (!p.mp3 && !p.opus && p.metadataBytes > 256 * 1024) {
    Serial.printf("[audio] FLAC: %lu KB of metadata (a picture?) read through before its first frame\n",
                  (unsigned long)(p.metadataBytes / 1024));
  }
  if (p.flacSeek) {
    char asked[16];
    mmssms(p.landedMs, asked, sizeof(asked));
    const int64_t t0 = esp_timer_get_time();
    const bool ok = flac_->seekTo(p.flacSample);
    const auto ms = static_cast<unsigned long>((esp_timer_get_time() - t0) / 1000);
    if (!ok) {
      // (Past the end of a file without a length, say.) libFLAC is left
      // in its seek error state: again from the top.
      Serial.printf("[audio] FLAC: %s asked: libFLAC couldn't seek there (%lu ms): from 0:00\n", asked, ms);
      closeDecoder();
      Prepared top;
      const std::string path = p.path;  // (p may be prepared_ itself)
      return prepare(path, StartAt{}, &top) && beginPrepared(top, out, trimmed);
    }
    Serial.printf("[audio] FLAC: starting %s in (libFLAC's seek to sample %llu, %lu ms)\n", asked,
                  (unsigned long long)p.flacSample, ms);
  }
  return true;
}

bool Core2AudioBackend::openDecoder(const std::string& path, AudioOutput* out, uint32_t startMs, uint32_t hintMs,
                                    bool trimmed) {
  Prepared& p = prepared_;
  StartAt at;
  at.ms = startMs;
  at.hintMs = hintMs;
  return prepare(path, at, &p) && !p.tone && beginPrepared(p, out, trimmed);
}

void Core2AudioBackend::planMp3(const uint8_t* probe, uint32_t got, uint32_t audioStart, const StartAt& at,
                                uint8_t* scratch, Prepared* p) {
  const uint32_t size = p->fileSize;
  const bool anchorsOn = gapless_.load(std::memory_order_relaxed) && gaplessTrim_.load(std::memory_order_relaxed);
  // The tail rule's length: the header's, exact (scaled down for a file
  // shorter than it says), else the bytes at the bitrate, else the hint.
  const uint32_t length =
      p->knownMs ? p->knownMs : trackseek::mp3LengthMs(probe, got, audioStart, size, at.hintMs);
  SourceReader reader(file_.get());
  trackseek::Plan plan;
  char asked[16], of[16];
  mmssms(at.ms, asked, sizeof(asked));
  mmss(length, of, sizeof(of));
  // 1. The resume point's anchor, checked against the file.
  const ResumeAnchor& a = at.anchor;
  if (a.valid()) {
    char why[64] = "";
    if (!anchorsOn) {
      snprintf(why, sizeof(why), "gapless trimming off");
    } else if (a.kind != ResumeAnchor::Kind::Mp3) {
      snprintf(why, sizeof(why), "%s", anchorKindName(a.kind));
    } else {
      const trackseek::AnchorCheck c = trackseek::checkAnchor(a, reader, size, p->firstAudio, length, scratch, &plan);
      switch (c) {
        case trackseek::AnchorCheck::Ok:
          break;
        case trackseek::AnchorCheck::Size:
          snprintf(why, sizeof(why), "the size: %lu -> %lu", (unsigned long)a.fileSize, (unsigned long)size);
          break;
        case trackseek::AnchorCheck::Frame:
          snprintf(why, sizeof(why), "the frame at %lu", (unsigned long)a.frameByte);
          break;
        case trackseek::AnchorCheck::Preroll:
          snprintf(why, sizeof(why), "the preroll at %lu", (unsigned long)a.prerollByte);
          break;
        default:
          snprintf(why, sizeof(why), "%s", trackseek::anchorCheckName(c));
          break;
      }
    }
    if (why[0]) Serial.printf("[audio] MP3: the resume anchor isn't this file's (%s): by its second\n", why);
  }
  // 2. The run index: a seek into what a run of this file decoded.
  if (!plan.ok() && anchorsOn && at.ms > 0 && p->lame.rate > 0 && trackseek::startMs(at.ms, length) != 0) {
    ResumeAnchor r;
    const uint64_t t = static_cast<uint64_t>(at.ms) * p->lame.rate / 1000;
    if (index_.find(p->pathHash, size, t, &r) &&
        trackseek::checkAnchor(r, reader, size, p->firstAudio, length, scratch, &plan) ==
            trackseek::AnchorCheck::Ok) {
      plan.source = trackseek::Source::Index;
    } else {
      plan = trackseek::Plan{};
    }
  }
  // 3-7. From the file: CBR arithmetic, LAME's TOC, another TOC or the
  // average bitrate and a chain of headers.
  if (!plan.ok() && at.ms > 0) {
    trackseek::PlanIn in;
    in.probe = probe;
    in.probeBytes = got;
    in.audioStart = audioStart;
    in.fileSize = size;
    in.hintMs = at.hintMs;
    in.targetMs = at.ms;
    in.lengthMs = length;
    in.useTag = anchorsOn && p->lame.lame;
    plan = trackseek::plan(in, reader, scratch);
  }
  if (!plan.ok()) {
    Serial.printf("[audio] MP3: %s asked, of %s: %s: from 0:00\n", asked, of,
                  trackseek::noPlanName(plan.why == trackseek::NoPlan::None ? trackseek::NoPlan::NoFrame : plan.why));
    return;
  }
  p->planned = true;
  p->plan = plan;
  p->from = plan.prerollByte;
  p->fromTop = false;
  p->landedMs = static_cast<uint32_t>(plan.sample * 1000 / plan.rate);
  p->startSample = plan.sample;
  p->startExact = plan.exact;
  char start[16];
  mmssms(p->landedMs, start, sizeof(start));
  Serial.printf("[audio] MP3: starting %s in, of %s (%s; %s): byte %lu, frame %lu + %lu samples\n", start, of,
                trackseek::sourceName(plan.source), plan.exact ? "exact" : "the time asked", (unsigned long)plan.prerollByte,
                (unsigned long)plan.landByte, (unsigned long)plan.skip);
}

void Core2AudioBackend::planOpus(OpusGenerator* g, const StartAt& at, Prepared* p) {
  // The plan is the reader's (lib/core/OggOpus, its class comment): a
  // bisection by page headers to the last page whose granule is at or
  // under the target less the preroll, the packets before the preroll
  // skipped by their TOCs, the preroll decoded and dropped, so the first
  // sample kept is the one asked, as a FLAC's. Its sources, the first that
  // gives one: the resume anchor (the FLAC model: the file's size, the
  // exact length and the tail rule, oggopus::checkAnchor()), planned with
  // the 600 ms resume preroll (the decoded PCM then matches a play from
  // the top, docs/OPUS.md section 7.3); else the millisecond with the
  // 200 ms seek preroll (oggopus::kSeekPrerollMs: section 10's measure),
  // the tail rule first (planStartMs(): the last 5 s and past the end
  // start at 0:00, as any start does). The plan reads page headers (a
  // 4 KB chunk a probe) and Q whole, which stays in hand for the start
  // (the device's seconds on the host, section 10: 4-7 probes and 8-20
  // reads / 30-90 KB on an mStream 128k file against M3's 11-26 reads;
  // ~37 reads / ~190 KB on the 510k file's 64 KB pages against 73 / 318),
  // here on the decode task before the decoder begins; the generator
  // decodes the preroll in its first passes (ten 20 ms frames: ~75 ms
  // at 240 MHz), so the first audio reaches the ring ~165-215 ms after
  // the request on a 128k file with the open from the cache (section
  // 10's model; M3 measured 290-460), the same `[audio] refill` line as
  // an MP3's seek measures it.
  const bool anchorsOn = gapless_.load(std::memory_order_relaxed) && gaplessTrim_.load(std::memory_order_relaxed);
  const uint32_t length = p->knownMs;
  char asked[16], of[16];
  mmssms(at.ms, asked, sizeof(asked));
  mmss(length, of, sizeof(of));
  oggopus::StartPlan plan;
  bool planned = false;
  const char* source = "the time asked";
  uint32_t prerollMs = oggopus::kSeekPrerollMs;
  // 1. The resume point's anchor, checked against the file as opened.
  const ResumeAnchor& a = at.anchor;
  if (a.valid()) {
    char why[64] = "";
    if (!anchorsOn) {
      snprintf(why, sizeof(why), "gapless trimming off");
    } else if (a.kind != ResumeAnchor::Kind::Opus) {
      snprintf(why, sizeof(why), "%s", anchorKindName(a.kind));
    } else {
      const oggopus::AnchorCheck c = oggopus::checkAnchor(a, p->fileSize, p->totalSamples);
      if (c == oggopus::AnchorCheck::Size) {
        snprintf(why, sizeof(why), "the size: %lu -> %lu", (unsigned long)a.fileSize, (unsigned long)p->fileSize);
      } else if (c == oggopus::AnchorCheck::Length) {
        snprintf(why, sizeof(why), "the length: %lu -> %lu samples%s", (unsigned long)a.frameHash,
                 (unsigned long)static_cast<uint32_t>(p->totalSamples), p->totalSamples == 0 ? " (not known)" : "");
      } else if (c != oggopus::AnchorCheck::Ok) {
        snprintf(why, sizeof(why), "%s", oggopus::anchorCheckName(c));
      }
    }
    if (why[0]) {
      Serial.printf("[audio] Opus: the resume anchor isn't this file's (%s): by its second\n", why);
    } else if (a.sample == 0) {
      return;  // (sample 0: from the top, as asked)
    } else {
      g->planStart(a.sample, oggopus::kResumePrerollSamples, &plan);
      prerollMs = oggopus::kResumePrerollMs;
      source = "its resume anchor";
      planned = true;
    }
  }
  // 2. The millisecond, the tail rule first.
  if (!planned && at.ms > 0) {
    if (length == 0) {
      // No last page found (a junk tail beyond the scan's windows): a plan
      // would land where asked and, past the end, end at once (the player
      // would move on), so from the top, as a FLAC without STREAMINFO.
      Serial.printf("[audio] Opus: %s asked: the length isn't known (no last page found): from 0:00\n", asked);
      return;
    }
    if (g->planStartMs(at.ms, oggopus::kSeekPrerollMs, &plan) == 0) {
      Serial.printf("[audio] Opus: %s asked, of %s: in its last %lu s or past its end: from 0:00\n", asked, of,
                    (unsigned long)(trackseek::kTailMs / 1000));
      return;
    }
    planned = true;
  }
  if (!planned || plan.target == 0) return;
  p->opusPlanned = true;
  p->opusPlan = plan;
  p->fromTop = false;
  p->landedMs = static_cast<uint32_t>(plan.target * 1000 / oggopus::kRate);
  p->startSample = plan.target;
  p->startExact = true;
  char start[16];
  mmssms(p->landedMs, start, sizeof(start));
  // The line names the plan the host runner makes for the same file and
  // second (tools/opus_check/opus_check.py plan: the same page, granule
  // and decode point, the plan being a function of the file), so a
  // device's landing can be compared with a PC decode without a PCM dump;
  // the generator's end line then says the sample it landed on.
  char where[96];
  if (plan.fromTop) {
    snprintf(where, sizeof(where), "from the first audio page (the target is inside its first second)");
  } else {
    snprintf(where, sizeof(where), "the page at byte %lu (granule %lld%s)", (unsigned long)plan.pageOffset,
             (long long)plan.k, plan.skipPage ? ", its own packets stepped over" : "");
  }
  Serial.printf("[audio] Opus: starting %s in, of %s (%s; exact): %s, decoding from %lld ms before (the %lu ms "
                "preroll), %lu probes, %lu reads / %lu KB in %lu ms\n",
                start, of, source, where, (long long)((plan.keepFrom - plan.decodeFrom) / (oggopus::kRate / 1000)),
                (unsigned long)prerollMs, (unsigned long)plan.probes, (unsigned long)plan.reads,
                (unsigned long)(plan.bytes / 1024), (unsigned long)(g->planUs() / 1000));
}

void Core2AudioBackend::beginRun(const Prepared& p) {
  recorder_.stop();
  // The trimmed timeline only with gapless trimming on (G1, Gt1); a
  // built-in track has none (it counts exactly from its ms).
  if (p.tone || !gapless_.load(std::memory_order_relaxed) || !gaplessTrim_.load(std::memory_order_relaxed)) return;
  SeekIndex::Run r;
  r.gen = runGen_;
  r.pathHash = p.pathHash;
  r.fileSize = p.fileSize;
  r.base = p.startSample;
  r.exact = p.startExact;
  if (!p.mp3) {
    // A FLAC's or an Opus track's run is its header alone: the anchor a
    // pause takes from it is the sample, the size and the length (the
    // FLAC model; an Opus length not known gives 0, which the anchor's
    // check then refuses).
    if (p.rate == 0) return;
    r.kind = p.opus ? SeekIndex::Kind::Opus : SeekIndex::Kind::Flac;
    r.rate = p.rate;
    r.totalSamples = p.totalSamples;
    index_.begin(r);
    return;
  }
  if (!p.lame.frame || p.lame.rate == 0) return;
  r.kind = SeekIndex::Kind::Mp3;
  r.rate = p.lame.rate;
  r.spf = p.lame.spf;
  if (p.planned) {
    // Its origin: the landing frame (the preroll the plan's), where the
    // plan's sample less its skip begins.
    r.origin = true;
    r.originByte = p.plan.landByte;
    r.originT0 = static_cast<int64_t>(p.plan.sample) - static_cast<int64_t>(p.plan.skip);
    r.originPreroll = p.plan.prerollByte;
    r.originHash = p.plan.landHash;
  } else if (p.from == p.firstAudio && p.firstAudio > 0) {
    // From the top: the first audio frame, inside the start trim.
    r.origin = true;
    r.originByte = p.firstAudio;
    r.originT0 = p.topT0;
    r.originPreroll = p.firstAudio;
    r.originHash = p.firstHash;
  }
  index_.begin(r);
  recorder_.begin(&index_, r.base, r.exact, p.planned);
}

void Core2AudioBackend::noteRun() {
  if (!recorder_.active() || !mp3_) return;
  switch (recorder_.afterPass(out_->trim(), *mp3_)) {
    case SeekRecorder::Settled::Late: {
      // The landing frame was lost: the start (and the time shown) a frame
      // later.
      const auto ms = static_cast<uint32_t>(recorder_.base() * 1000 / prepared_.lame.rate);
      startMs_.store(ms, std::memory_order_relaxed);
      if (engine_->decodingToken() == 0) engine_->setStartMs(ms);
      Serial.printf("[audio] MP3: the landing frame was lost: started %lu samples later\n",
                    (unsigned long)out_->trim().lateBy());
      break;
    }
    case SeekRecorder::Settled::Elsewhere:
      Serial.println("[audio] MP3: the start landed on another frame than planned (damaged data?): not exact");
      break;
    case SeekRecorder::Settled::None:
      break;
  }
}

PinnedMp3* Core2AudioBackend::makeMp3() {
  mp3_.reset();  // the track before's generator (it gave its state back when it stopped; else here)
  const char* why = !mp3Arena_.attached() ? "no pinned block" : mp3Arena_.inUse() ? "the pinned block is in use" : "";
  bool pinned = false;
  PinnedMp3* g = PinnedMp3::make(mp3Arena_, &pinned);
  mp3Pinned_ = g && pinned;
  if (g && !pinned) {
    ++mp3Unpinned_;
    Serial.printf("[audio] MP3: libmad's state malloc'd for this track (%s; %lu so far): its speed depends on "
                  "where it lands\n",
                  why, (unsigned long)mp3Unpinned_);
  }
  if (!g) Serial.println("[audio] MP3: no RAM for libmad's state");
  return g;
}

void Core2AudioBackend::describeMp3State(char* buf, size_t size) const {
  if (!mp3Arena_.attached()) {
    snprintf(buf, size, "not pinned (no PSRAM block): malloc'd per track");
    return;
  }
  const void* b = mp3Arena_.block();
  snprintf(buf, size, "pinned at %p, %s", b,
           DecoderArena::whereName(DecoderArena::where(b, mp3Arena_.bytes())));
}

OpusGenerator* Core2AudioBackend::opusGenerator() {
  if (!opus_) opus_.reset(new OpusGenerator(mp3Arena_, opusLayout_));  // ~1 KB: internal RAM
  return opus_.get();
}

// ---- the Opus open cache (docs/OPUS.md section 10) ----

bool Core2AudioBackend::findOpusRecord(const std::string& path, uint32_t fileSize, oggopus::OpenRecord* out) {
  std::lock_guard<std::mutex> guard(opusCacheLock_);
  return opusCache_.find(OpusOpenCache::hashPath(path.c_str()), fileSize, out);
}

void Core2AudioBackend::putOpusRecord(const std::string& path, const oggopus::OpenRecord& rec) {
  std::lock_guard<std::mutex> guard(opusCacheLock_);
  opusCache_.put(OpusOpenCache::hashPath(path.c_str()), rec);
  opusCacheDueMs_ = millis() + kOpusCacheSaveDelayMs;
}

void Core2AudioBackend::forgetOpusRecord(const std::string& path, uint32_t fileSize) {
  std::lock_guard<std::mutex> guard(opusCacheLock_);
  opusCache_.forget(OpusOpenCache::hashPath(path.c_str()), fileSize);
  opusCacheDueMs_ = millis() + kOpusCacheSaveDelayMs;
}

void Core2AudioBackend::loadOpusCache() {
  if (opusCachePath_.empty() || opusCache_.capacity() == 0) return;
  const int64_t t0 = esp_timer_get_time();
  OpusOpenCache::Load r = OpusOpenCache::Load::Empty;
  File f = fs_->open(opusCachePath_.c_str(), FILE_READ);
  if (f) {
    FileSource src(f);
    std::lock_guard<std::mutex> guard(opusCacheLock_);
    r = opusCache_.load(src);
    f.close();
  }
  Serial.printf("[opus] the open cache: %lu of %lu entries (%lu B of PSRAM) %s %s in %lu ms\n",
                (unsigned long)opusCache_.size(), (unsigned long)opusCache_.capacity(),
                (unsigned long)opusCache_.bytes(), OpusOpenCache::loadName(r), opusCachePath_.c_str(),
                (unsigned long)((esp_timer_get_time() - t0) / 1000));
}

void Core2AudioBackend::saveOpusCache() {
  // The blob made under the lock (microseconds, into PSRAM), the file
  // written outside it: aside, then swapped in, so a cut-off write never
  // leaves a cache that looks whole (and load() checks its sum anyway).
  MemorySink blob(psramAlloc, psramFree);
  uint32_t entries = 0;
  {
    std::lock_guard<std::mutex> guard(opusCacheLock_);
    if (!opusCache_.save(blob)) return;  // (no memory: still dirty, tried again next time)
    entries = opusCache_.size();
  }
  const int64_t t0 = esp_timer_get_time();
  const std::string tmp = opusCachePath_ + ".tmp";
  File f = fs_->open(tmp.c_str(), FILE_WRITE);
  bool ok = static_cast<bool>(f);
  if (ok) {
    FileSink sink(f);
    ok = sink.write(blob.data(), blob.size());
    f.close();
  }
  if (ok) {
    fs_->remove(opusCachePath_.c_str());
    ok = fs_->rename(tmp.c_str(), opusCachePath_.c_str());
  } else {
    fs_->remove(tmp.c_str());
  }
  if (ok) {
    ++opusCacheSaves_;
  } else {
    // save() cleaned the cache as it made the blob, which the card never
    // got (the temp file wouldn't open: the mount's few file handles all
    // taken by the track, the queue's write and the thumbnails; a short
    // write; the rename): dirty again, and due in kOpusCacheRetryMs, so
    // the record reaches the card before a boot needs it, without a
    // further open to prompt it. A put meanwhile has dirtied it already
    // and brought the time forward: either is fine.
    ++opusCacheSaveFails_;
    std::lock_guard<std::mutex> guard(opusCacheLock_);
    opusCache_.markDirty();
    opusCacheDueMs_ = millis() + kOpusCacheRetryMs;
  }
  Serial.printf("[opus] the open cache %s %s: %lu entries, %lu B, %lu ms (saves %lu, failed %lu)\n",
                ok ? "saved to" : "couldn't be saved to", opusCachePath_.c_str(), (unsigned long)entries,
                (unsigned long)blob.size(), (unsigned long)((esp_timer_get_time() - t0) / 1000),
                (unsigned long)opusCacheSaves_, (unsigned long)opusCacheSaveFails_);
}

void Core2AudioBackend::closeDecoder() {
  // Always stop(), even when the decoder already says it isn't running: at the
  // end of a file AudioGeneratorFLAC clears `running` itself but only stop()
  // deletes its libFLAC decoder (~100 KB PSRAM + ~2.5 KB internal per track).
  // stop() is safe to repeat for both the MP3 and FLAC generators.
  if (decoder_) decoder_->stop();
  decoder_ = nullptr;
  recorder_.stop();
  toneTrack_ = false;
  clickTrack_ = false;
  if (file_) file_->close();
}

// ---- gapless playback: the decode task's side (GaplessEngine::Tracks) ----

bool Core2AudioBackend::probe(const GaplessJoin::Offer& next, uint32_t* rate) {
  probeUs_ = esp_timer_get_time();
  StartAt at;
  at.hintMs = next.hintMs;
  if (!prepare(next.path, at, &prepared_)) return false;
  *rate = prepared_.rate;
  return true;
}

bool Core2AudioBackend::start() {
  // The heard track's description stays until the join is heard.
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!heardOverride_) {
      heardDescription_ = description_;
      heardOverride_ = true;
    }
  }
  tablesKeep = true;  // a chain of joins: the tables' copy stays until the next request
  index_.beginJoin();  // its run in the other slot: the heard one's stays until the advance
  if (!beginPrepared(prepared_, out_.get(), true)) {
    index_.cut();
    return false;
  }
  openMs_ = static_cast<uint32_t>((esp_timer_get_time() - probeUs_) / 1000);
  return true;
}

void Core2AudioBackend::close() { closeDecoder(); }

void Core2AudioBackend::note(const GaplessEngine::Note& n) {
  const float ms = static_cast<int32_t>(n.ringFrames) * 1000.0f / kRingRate;
  switch (n.event) {
    case GaplessEngine::Event::Joined:
    case GaplessEngine::Event::JoinedReset:
      Serial.printf("[gapless] decoding ahead: %s (%lu Hz, %s) with %.0f ms of the track before left; opened in %lu "
                    "ms\n",
                    prepared_.path.c_str(), (unsigned long)n.rate,
                    n.event == GaplessEngine::Event::Joined ? "the same rate: one stream"
                    : n.late                                ? "late: after the tail"
                                                            : "another rate: after the tail",
                    ms, (unsigned long)openMs_);
      if (prepared_.metadataBytes > 1024 * 1024) {
        Serial.printf("[gapless] (its %lu KB of metadata were read through at the open: an underrun risk)\n",
                      (unsigned long)(prepared_.metadataBytes / 1024));
      }
      break;
    case GaplessEngine::Event::OpenFailed:
      Serial.printf("[gapless] can't decode ahead %s; the track before ends as before\n", prepared_.path.c_str());
      break;
    case GaplessEngine::Event::EmptyAhead:
      Serial.printf("[gapless] %s gave no audio: taken back out\n", prepared_.path.c_str());
      break;
    case GaplessEngine::Event::Cut: {
      index_.cut();  // the cut track's run goes with it
      const int64_t asked = nextAtUs_.load(std::memory_order_relaxed);
      const uint32_t us = asked > 0 ? static_cast<uint32_t>(esp_timer_get_time() - asked) : 0;
      cutLatencyUs_.store(us, std::memory_order_relaxed);
      if (us > cutLatencyMaxUs_.load(std::memory_order_relaxed)) cutLatencyMaxUs_.store(us, std::memory_order_relaxed);
      Serial.printf("[gapless] cut: what comes next changed: the track decoded ahead taken back out, %.0f ms before "
                    "the join (%lu us after the word)\n",
                    ms, (unsigned long)us);
      break;
    }
    case GaplessEngine::Event::CutTooLate:
      Serial.println("[gapless] too late to cut: the outputs are past the join; the change applies once it is heard");
      break;
  }
}

Core2AudioBackend::Produced Core2AudioBackend::produceTone() {
  RingFeed& f = feed();
  if (toneAt_ == toneN_) {  // the last chunk is all in: a new one
    if (ring_->space() < f.roomFor(kChunkFrames)) {  // ring full: the output is ~1.5 s behind us
      noteStartProgress(true);
      rest(10);
      return Produced::More;
    }
    toneN_ = clickTrack_ ? click_.generate(chunk_, kChunkFrames) : tone_.generate(chunk_, kChunkFrames);
    toneAt_ = 0;
    if (toneN_ == 0) return Produced::Done;  // its end
  }
  // Through RingOutput's converter at the tone's rate. What the ring has no
  // room for stays in chunk_ for the next pass (the generators have moved
  // past it), the way a decoder keeps the sample it couldn't hand over. A
  // pass makes at most kChunkFrames ring frames, as a file's does: an 8 kHz
  // tone's whole chunk would be 5.5 times that.
  const int64_t t0 = esp_timer_get_time();
  const uint64_t before = f.made();
  const uint32_t taken = f.write(chunk_ + 2 * toneAt_, toneN_ - toneAt_, kChunkFrames);
  toneAt_ += taken;
  f.commit();
  const auto toneUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  busyUs_ += toneUs;
  busyTotalUs_ += toneUs;
  producedFrames_ += f.made() - before;
  publishRate();
  if (!shared_.expectingAudio && producedFrames_ >= kRingRate / 4) shared_.expectingAudio = true;
  noteRingFill();
  noteStartProgress(taken == 0);
  // Share core 1 with the UI loop, paced like a file (produceDecoded()):
  // flat out up to 500 ms of ring, then at most 1.5x realtime until the
  // ring is first full, so a test tone's start can't starve the UI.
  RefillPacer::Config pace = refillPacing();
  pace.enabled = pace.enabled && fullMs_.load(std::memory_order_relaxed) < 0;
  rest(taken == 0 ? 10
                  : RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(f.made() - before), kRingRate,
                                         static_cast<uint32_t>(toneUs)));
  return Produced::More;
}

void Core2AudioBackend::noteStartProgress(bool full) {
  if (fullMs_.load(std::memory_order_relaxed) >= 0) return;  // done for this start
  const auto since = static_cast<int32_t>(millis() - requestMs_.load(std::memory_order_relaxed));
  const uint32_t bufferedMs = bufferedMsNow();
  if (firstAudioMs_.load(std::memory_order_relaxed) < 0 && producedFrames_ > 0) firstAudioMs_ = since;
  if (ring500Ms_.load(std::memory_order_relaxed) < 0 && bufferedMs >= 500) ring500Ms_ = since;
  if (steadyMs_.load(std::memory_order_relaxed) < 0 && bufferedMs >= kSteadyMs) steadyMs_ = since;
  if (full) fullMs_ = since;
}

void Core2AudioBackend::noteRingFill() {
  if (ringSteady_.load(std::memory_order_relaxed)) return;
  if (static_cast<uint64_t>(ring_->size()) * 1000 >= static_cast<uint64_t>(kSteadyMs) * kRingRate) ringSteady_ = true;
}

Core2AudioBackend::Produced Core2AudioBackend::produceDecoded() {
  RingFeed& f = feed();
  // Room for a full pass, in ring frames (at the track's ratio), plus what
  // RingOutput may already be holding.
  if (ring_->space() < f.roomFor(2 * kChunkFrames)) {
    noteStartProgress(true);
    rest(10);
    return Produced::More;
  }

  const int64_t t0 = esp_timer_get_time();
  const uint64_t before = producedFrames_;
  const uint64_t madeBefore = f.made();
  // Where the file was when the first audio came (the tags in front are
  // left out of the length estimate), and where it is now.
  if (before == 0) srcPos0_.store(file_->getPos(), std::memory_order_relaxed);
  // At most kChunkFrames source frames (the decode work per pass as at
  // 44.1 kHz) and kChunkFrames ring frames (the conversion's: an 8 kHz
  // file's 1024 source frames would make 5,645, ~20 ms above the UI loop).
  f.setBudget(kChunkFrames);
  const bool running = decoder_->loop();
  f.commit();
  srcPos_.store(file_->getPos(), std::memory_order_relaxed);
  srcSize_.store(file_->getSize(), std::memory_order_relaxed);
  const auto passUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  busyUs_ += passUs;
  busyTotalUs_ += passUs;
  raiseTo(passPeakUs_, static_cast<uint32_t>(passUs));  // the card worker's yield (ScanScheduler, DecodePass)
  if (passUs > maxPassUs_.load(std::memory_order_relaxed)) {
    maxPassUs_.store(static_cast<uint32_t>(passUs));
    // The heard track's figure too (the console's pass_max: docs/OPUS.md
    // gate G6 wants a track's own) while this track is the one heard. A
    // join's begin starts maxPassUs_ over while the track before is still
    // heard, so the figure a track makes decoding ahead waits there until
    // takeAdvance() takes it. The store above then the load here, against
    // takeAdvance()'s store of the token then its load of the figure (both
    // sequentially consistent): one side sees the other's write, so the
    // pass reaches the heard figure from one side or the other. Raised, not
    // stored: takeAdvance()'s own store of the figure can land after this
    // write and lower it (its raise after undoes that), and this side must
    // never lower what either wrote (the M3 review's finding).
    if (engine_->decodingToken() == heardToken_.load()) raiseTo(heardMaxPassUs_, static_cast<uint32_t>(passUs));
  }
  producedFrames_ = before + (f.made() - madeBefore);  // ring frames, 44.1 kHz
  publishRate();

  if (f.rejected()) {
    // The request's own track refused before its first frame: it fails, as
    // before. A track joined, or one that has played: an early end (what
    // it gave plays; docs/GAPLESS.md section 5.3).
    if (engine_->decodingToken() == 0 && producedFrames_ == 0) return Produced::Failed;
    endedEarly(refusalText());
    return Produced::Done;
  }
  if (!described_ && f.rate() > 0) {
    setText(description_, std::string(codec_) + ", " + std::to_string(f.rate()) + " Hz");
    described_ = true;
    if (f.rate() != static_cast<int>(kRingRate)) {
      Serial.printf("[audio] %d Hz -> %lu Hz (%s)\n", f.rate(), (unsigned long)kRingRate,
                    f.converter().currentPlan().route);
    }
  }
  if (!shared_.expectingAudio && producedFrames_ >= kRingRate / 4) {
    shared_.expectingAudio = true;  // past the pre-roll
  }
  noteRingFill();
  noteStartProgress(false);
  if (running) noteRun();  // (the pass ended on a refused sample: the cursor's)
  if (!running) {
    // Its end. Before the file's (a decode error the generator gave up on:
    // libmad's, which closes the file too): what the trim holds is real
    // audio, not the padding. The Opus generator knows its own end (the
    // EOS trim, a file cut short, another stream after ours): the file's
    // position says nothing there (a trailing stream, junk after the end).
    if (prepared_.opus && opus_) {
      opus_->logEnd("end");
      if (opus_->endedEarly()) {
        endedEarly(opus_->endText());
      } else {
        early_ = false;
      }
    } else {
      early_ = !file_->isOpen() || file_->getPos() < file_->getSize();
    }
    return Produced::Done;
  }
  // Share core 1 with the UI loop: 1 ms, or, with refill pacing on, during
  // the fill after a start or skip (until the ring is first full: fullMs_
  // is -1 until then) and past 500 ms, long enough to keep this track under
  // the rate cap (see setRefillPacing()). Later dips (an SD stall, a
  // Bluetooth burst) refill flat out, as without pacing.
  RefillPacer::Config pace = refillPacing();
  pace.enabled = pace.enabled && fullMs_.load(std::memory_order_relaxed) < 0;
  rest(RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(producedFrames_ - before), kRingRate,
                            static_cast<uint32_t>(passUs)));
  return Produced::More;
}

void Core2AudioBackend::runBench(const std::string& path) {
  CountingOutput counter;
  if (!openDecoder(path, &counter, 0, 0, false)) {
    Serial.printf("[bench] can't decode %s\n", path.c_str());
    return;
  }
  // Times only the decoding, and yields between bursts so the UI loop runs.
  int64_t busyUs = 0;
  for (;;) {
    counter.budget = CountingOutput::kBurst;
    const int64_t t0 = esp_timer_get_time();
    const bool more = decoder_->loop();
    busyUs += esp_timer_get_time() - t0;
    if (!more) break;
    if (counter.rate > 0 && counter.frames >= static_cast<uint64_t>(counter.rate) * kBenchSeconds) break;
    vTaskDelay(1);
  }
  const double seconds = busyUs / 1e6;
  const bool opus = prepared_.opus && opus_;
  if (opus) opus_->logEnd("bench, decode only");  // (its counters go with the close)
  closeDecoder();
  const double audio = counter.rate > 0 ? static_cast<double>(counter.frames) / counter.rate : 0;
  Serial.printf("[bench] %s: %.1f s of %d Hz audio in %.2f s = %.1fx realtime (%.1f%% of a core), "
                "decode stack free %lu\n",
                path.c_str(), audio, counter.rate, seconds, seconds > 0 ? audio / seconds : 0.0,
                audio > 0 ? 100.0 * seconds / audio : 0.0,
                (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
  if (std::strcmp(codec_, "MP3") == 0) {
    // Its speed depends on where libmad's state is (RESAMPLER.md section 10d).
    char where[96];
    describeMp3State(where, sizeof(where));
    if (!mp3Pinned_) snprintf(where, sizeof(where), "malloc'd for this track, not pinned");
    Serial.printf("[bench] libmad's state: %s\n", where);
  }
  if (counter.rate <= 0 || counter.rate == static_cast<int>(kRingRate) || audio <= 0) return;

  // Another rate: the same stretch again, decoded and converted through
  // RingOutput (its output dropped), for what the two cost together, cache
  // misses included (Rb times the converter alone).
  RingFeed& f = feed();
  f.reset(cpuMhz(), /*hiRes=*/true);  // measured even while 88.2/96 kHz don't play (RateConverter::kHiResOn)
  if (!openDecoder(path, out_.get(), 0, 0, false)) return;
  f.setDiscard(true);
  const uint64_t want = counter.frames;
  uint64_t taken = 0;
  busyUs = 0;
  for (;;) {
    f.setBudget(CountingOutput::kBurst);
    const int64_t t0 = esp_timer_get_time();
    const bool more = decoder_->loop();
    f.commit();
    busyUs += esp_timer_get_time() - t0;
    taken += CountingOutput::kBurst - f.budgetLeft();
    if (!more || f.rejected() || taken >= want) break;
    vTaskDelay(1);
  }
  f.setDiscard(false);
  if (opus) opus_->logEnd("bench, decode + convert");
  closeDecoder();
  if (f.rejected()) {
    Serial.printf("[bench] %s: not converted: %s\n", path.c_str(), refusalText().c_str());
    return;
  }
  const double both = busyUs / 1e6;
  const double audio2 = static_cast<double>(taken) / counter.rate;
  if (audio2 <= 0) return;
  Serial.printf("[bench] %s: decode + convert to %lu Hz (%s) in %.2f s = %.1f%% of a core at %lu MHz: the converter "
                "%.1f%% in the decoder's company\n",
                path.c_str(), (unsigned long)kRingRate, f.converter().currentPlan().route, both, 100.0 * both / audio2,
                (unsigned long)getCpuFrequencyMhz(), 100.0 * both / audio2 - 100.0 * seconds / audio);
}

bool Core2AudioBackend::checkKernel(const char* when) {
  if (!RateConverter::fastKernelBuilt()) {
    Serial.println("[rate] kernel: C (this build has no fast kernel)");
    return false;
  }
  const uint32_t seed = esp_random();
  const uint32_t c0 = esp_cpu_get_cycle_count();
  const RateConverter::SelfTest t = RateConverter::kernelSelfTest(seed, 4);
  const uint32_t us = (esp_cpu_get_cycle_count() - c0) / getCpuFrequencyMhz();
  if (t.mismatches != 0) kernelFailed_ = true;
  RateConverter::useFastKernel(!kernelFailed_);
  if (t.mismatches == 0) {
    Serial.printf("[rate] kernel: %s; the MAC16 self-test %s gave the C kernel's bits in all %lu dot products "
                  "(seed %08lx, %lu us)\n",
                  kernelFailed_ ? "C (MAC16 failed before: until a restart)" : "MAC16", when, (unsigned long)t.dots,
                  (unsigned long)seed, (unsigned long)us);
  } else {
    Serial.printf("[rate] kernel: MAC16 FAILED its self-test %s: %lu of %lu dot products differ from the C kernel "
                  "(the first: C %ld, MAC16 %ld; seed %08lx). The C kernel from now on, until a restart\n",
                  when, (unsigned long)t.mismatches, (unsigned long)t.dots, (long)t.wantC, (long)t.gotFast,
                  (unsigned long)seed);
  }
  return !kernelFailed_;
}

bool Core2AudioBackend::checkRoutes() {
  if (!RateConverter::fastKernelBuilt()) return true;
  RateConverter& c = feed().benchConverter();
  // Full-scale noise (the clamps too) in chunk_'s first half, the output
  // in its second: blocks of 1-64 frames make at most 64 x 6 + 8 frames.
  constexpr uint32_t kIn = kChunkFrames / 2;
  constexpr uint32_t kFrames = 8192;
  int16_t* out = chunk_ + 2 * kIn;
  uint32_t seed = 4321;
  for (uint32_t i = 0; i < 2 * kIn; ++i) {
    seed = seed * 1664525u + 1013904223u;
    chunk_[i] = static_cast<int16_t>(seed >> 16);
  }
  const bool fast = RateConverter::fastKernel();
  bool same = true;
  uint32_t routes = 0;
  // About 260 M cycles in all (the C kernel at the low rates most of it):
  // the UI loop runs every 5 ms of it, as between the bench's blocks.
  const uint32_t sliceCycles = getCpuFrequencyMhz() * 5000;
  uint32_t sliceStart = esp_cpu_get_cycle_count();
  for (const uint32_t hz : kRateBenchRates) {
    uint32_t hash[2] = {0, 0}, made[2] = {0, 0}, clamped[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
      RateConverter::useFastKernel(k == 1);
      c.reset();
      if (!c.setRate(hz, RateConverter::kHiResMinMhz, /*hiRes=*/true)) break;
      uint32_t h = 2166136261u, total = 0, blocks = 77;
      auto mix = [&h](const int16_t* p, uint32_t frames) {
        for (uint32_t i = 0; i < 2 * frames; ++i) h = (h ^ static_cast<uint16_t>(p[i])) * 16777619u;
      };
      for (uint32_t done = 0; done < kFrames;) {
        blocks = blocks * 1664525u + 1013904223u;
        const uint32_t n = 1 + (blocks >> 26);  // 1..64
        const uint32_t w = c.convert(chunk_ + 2 * (done % (kIn - 64)), n, out);
        mix(out, w);
        total += w;
        done += n;
        if (esp_cpu_get_cycle_count() - sliceStart >= sliceCycles) {
          vTaskDelay(1);
          sliceStart = esp_cpu_get_cycle_count();
        }
      }
      while (!c.finished()) {
        const uint32_t w = c.finishPush(out);
        mix(out, w);
        total += w;
      }
      hash[k] = h;
      made[k] = total;
      clamped[k] = c.clamped();
    }
    ++routes;
    if (hash[0] != hash[1] || made[0] != made[1] || clamped[0] != clamped[1]) {
      same = false;
      Serial.printf("[rate bench] route check: %lu Hz DIFFERS: C %08lx (%lu frames, %lu clamped), MAC16 %08lx (%lu, %lu)\n",
                    (unsigned long)hz, (unsigned long)hash[0], (unsigned long)made[0], (unsigned long)clamped[0],
                    (unsigned long)hash[1], (unsigned long)made[1], (unsigned long)clamped[1]);
    }
  }
  c.reset();
  if (!same) kernelFailed_ = true;
  RateConverter::useFastKernel(fast && !kernelFailed_);
  Serial.printf("[rate bench] route check: %lu routes, %lu frames each in blocks of 1-64, full-scale noise: MAC16 %s\n",
                (unsigned long)routes, (unsigned long)kFrames,
                same ? "identical to C, bit for bit" : "DIFFERS: the C kernel until a restart");
  return same;
}

void Core2AudioBackend::benchConsume(uint32_t hz) {
  RingFeed& f = feed();
  f.reset(RateConverter::kHiResMinMhz, /*hiRes=*/true);
  f.setDiscard(true);
  if (!f.setRate(static_cast<int>(hz))) return;
  AudioOutput* o = out_.get();  // the generators' virtual call, as they make it
  const uint32_t frames = 2 * hz;  // 2 s of audio
  uint32_t cycles = 0, done = 0, blocks = 0;
  int16_t s[2];
  while (done < frames) {
    f.setBudget(kChunkFrames);
    const uint32_t c0 = esp_cpu_get_cycle_count();
    uint32_t i = 0;
    for (; i < kChunkFrames; ++i) {
      const uint32_t at = 2 * ((done + i) % kChunkFrames);
      s[0] = chunk_[at];
      s[1] = chunk_[at + 1];
      if (!o->ConsumeSample(s)) break;
    }
    f.commit();
    cycles += esp_cpu_get_cycle_count() - c0;
    done += i;
    if (i == 0) break;  // (can't happen: discarding)
    if (++blocks % 8 == 0) vTaskDelay(1);
  }
  f.setDiscard(false);
  Serial.printf("[rate bench] ConsumeSample() a frame at a time at %6lu Hz (%s): %.0f cycles per source frame "
                "(the copy from PSRAM included), %.1f M cycles per second of audio = %.1f%% of a core at %lu MHz\n",
                (unsigned long)hz, f.converter().currentPlan().route, done ? static_cast<double>(cycles) / done : 0.0,
                static_cast<double>(cycles) / 2 / 1e6, 100.0 * cycles / 2 / (getCpuFrequencyMhz() * 1e6),
                (unsigned long)getCpuFrequencyMhz());
}

void Core2AudioBackend::benchKernel() {
  alignas(4) int16_t window[resampler::kTaps + 2];
  int16_t* x = window + 1;  // as the converter's windows: 2 bytes past a 4-byte boundary
  alignas(4) int16_t rowRam[resampler::kTaps];
  for (int j = 0; j < resampler::kTaps; ++j) x[j] = chunk_[j];
  std::memcpy(rowRam, resampler::kD147[37], sizeof(rowRam));
  constexpr uint32_t kDots = 20000;
  struct Case {
    const char* what;
    int32_t (*dot)(const int16_t*, const int16_t*);
    int rows;  // 0: rowRam; 1: one flash row; else walk the stored rows like 147/160
  };
  const Case cases[] = {
      {"C, flash, the table walked", RateConverter::dotC, resampler::kD147Stored},
      {"C, internal RAM row", RateConverter::dotC, 0},
      {"MAC16, flash, the table walked", RateConverter::dotFast, resampler::kD147Stored},
      {"MAC16, flash, one row", RateConverter::dotFast, 1},
      {"MAC16, internal RAM row", RateConverter::dotFast, 0},
  };
  int32_t sink = 0;
  for (const Case& k : cases) {
    if (k.dot == RateConverter::dotFast && !RateConverter::fastKernelBuilt()) continue;
    uint32_t row = 0;
    const uint32_t c0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < kDots; ++i) {
      const int16_t* c = k.rows == 0 ? rowRam : resampler::kD147[k.rows == 1 ? 37 : row];
      sink += k.dot(c, x);
      row += 13;  // 160 mod 147: the order 147/160 visits its rows in
      if (row >= static_cast<uint32_t>(resampler::kD147Stored)) row -= resampler::kD147Stored;
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
    Serial.printf("[rate bench] kernel: %-32s %5.1f cycles per 48-tap dot product (%.2f per multiply, the call "
                  "included)\n",
                  k.what, static_cast<double>(cycles) / kDots, static_cast<double>(cycles) / kDots / resampler::kTaps);
    vTaskDelay(1);
  }
  if (sink == 0x7fffffff) Serial.println("");  // keeps the loops
}

void Core2AudioBackend::runRateBench() {
  RingFeed& f = feed();
  const uint32_t mhz = getCpuFrequencyMhz();
  const uint32_t setting = cpuMhz();
  Serial.printf("[rate bench] %lu s of stereo audio per rate through RingOutput's converter on the decode task, "
                "output dropped; CPU %lu MHz now (set at boot: %lu MHz)\n",
                (unsigned long)kRateBenchSeconds, (unsigned long)mhz, (unsigned long)setting);
  // First the fast kernel against the C one: the dot products, then whole
  // routes. A mismatch turns the fast kernel off.
  checkKernel("in Rb");
  checkRoutes();
  benchKernel();
  const bool fast = RateConverter::fastKernel();
  // A fixed stereo signal (noise at about -12 dBFS, the same every run):
  // the kernel's cost doesn't depend on the values.
  uint32_t seed = 12345;
  for (uint32_t i = 0; i < 2 * kChunkFrames; ++i) {
    seed = seed * 1664525u + 1013904223u;
    chunk_[i] = static_cast<int16_t>(static_cast<int32_t>(seed >> 16) / 4 - 8192);
  }
  for (const uint32_t hz : kRateBenchRates) {
    // Every route is measured, the hi-res ones at 160 MHz too (to settle
    // that rule) and while they don't play (kHiResOn): planned as at 240 MHz.
    // With each kernel: C, then MAC16 when it is on.
    double perSecond[2] = {0, 0};
    for (int k = 0; k < (fast ? 2 : 1); ++k) {
      RateConverter::useFastKernel(k == 1);
      f.reset(RateConverter::kHiResMinMhz, /*hiRes=*/true);
      f.setDiscard(true);
      if (!f.setRate(static_cast<int>(hz))) break;
      uint64_t cycles = 0;
      uint32_t left = hz * kRateBenchSeconds;
      uint32_t blocks = 0;
      while (left > 0) {
        const uint32_t n = left < kChunkFrames ? left : kChunkFrames;
        const uint32_t c0 = esp_cpu_get_cycle_count();
        const uint32_t taken = f.write(chunk_, n);
        f.commit();
        cycles += esp_cpu_get_cycle_count() - c0;  // per block: no 32-bit wrap
        left -= taken;
        if (taken == 0) break;  // (can't happen: discarding, the stage always has room)
        if (++blocks % 4 == 0) vTaskDelay(1);  // the UI loop runs between
      }
      perSecond[k] = static_cast<double>(cycles) / kRateBenchSeconds;
    }
    RateConverter::useFastKernel(fast);
    const RateConverter::Plan p = RateConverter::plan(hz, setting);
    char fastText[48] = "";
    if (fast) {
      snprintf(fastText, sizeof(fastText), "; MAC16 %5.1f M = %4.1f%%", perSecond[1] / 1e6,
               100.0 * perSecond[1] / (mhz * 1e6));
    }
    Serial.printf("[rate bench] %6lu Hz (%s): C %5.1f M cycles per second of audio = %4.1f%%%s of a core at %lu MHz%s%s%s\n",
                  (unsigned long)hz, f.converter().currentPlan().route, perSecond[0] / 1e6,
                  100.0 * perSecond[0] / (mhz * 1e6), fastText, (unsigned long)mhz, p.ok ? "" : " (not played: it ",
                  p.ok ? "" : p.reason, p.ok ? "" : ")");
  }
  f.setDiscard(false);
  // The generators' own path, a frame per call: the passthrough every
  // 44.1 kHz track takes, and the block path at 48 kHz.
  benchConsume(44100);
  benchConsume(48000);
  RateConverter::useFastKernel(fast);
  Serial.printf("[rate bench] cycles include interrupts and anything that preempted the decode task; "
                "b<n> on a file at another rate gives decode + convert together. The kernel now: %s\n",
                RateConverter::fastKernel() ? "MAC16" : "C");
}
