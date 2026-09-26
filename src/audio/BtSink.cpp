#include "audio/BtSink.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_timer.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "BtControl.h"
#include "ReconnectPlanner.h"
#include "StreamRestart.h"
#include "VolumeMath.h"

namespace {
BtSink* sink = nullptr;  // ESP32-A2DP takes plain function pointers

// Without a configured name, only connect to a device this close (dBm). -70
// wasn't enough: a TV in the next room fluctuated past it and got paired.
constexpr int kMinRssi = -55;
// Reconnect attempts (~10 s apart) to the remembered device before scanning.
// The library's default of 1000 blocks discovery for hours if it's gone.
constexpr int kReconnectTries = 3;
// BtAppT runs our handlers, which log and read/write NVS: the library's
// default of 3072 bytes is too tight (watch appTaskStackFree in the stats).
constexpr int kAppTaskStack = 6144;
constexpr UBaseType_t kEventQueueLength = 16;

// How often the loop lets BtAppT check its timers (stream retries, the delayed
// suspend, command and probe timeouts).
constexpr uint32_t kTickMs = 250;
// How long a pause from the headphones' key may take to reach the transport
// before the prompt suspend it asked for is dropped.
constexpr uint32_t kSuspendPromptlyForMs = 1000;
constexpr const char* kPrefsNamespace = "player";
constexpr const char* kPrefsSinkName = "bt_name";

bool containsIgnoreCase(const char* haystack, const char* needle) {
  const size_t n = std::strlen(needle);
  for (const char* h = haystack; *h; ++h) {
    size_t i = 0;
    while (i < n && h[i] &&
           std::tolower(static_cast<unsigned char>(h[i])) ==
               std::tolower(static_cast<unsigned char>(needle[i]))) {
      ++i;
    }
    if (i == n) return true;
  }
  return false;
}

// Name of the one bit set in an SBC configuration field (bit 3 first), or "?".
// Bit values: esp_a2dp_api.h ESP_A2D_SBC_CIE_*.
const char* bitName(unsigned bits, const char* b3, const char* b2, const char* b1, const char* b0) {
  switch (bits) {
    case 0x8: return b3;
    case 0x4: return b2;
    case 0x2: return b1;
    case 0x1: return b0;
    default: return "?";
  }
}

const char* controlName(AbsVolumePolicy::Mode m) {
  switch (m) {
    case AbsVolumePolicy::Mode::Absolute: return "headphones";
    case AbsVolumePolicy::Mode::Probing: return "asking";
    case AbsVolumePolicy::Mode::Software: break;
  }
  return "software";
}

constexpr uint8_t kHeldOffBit = 0x80;

const char* streamName(uint8_t published) {
  if (published & kHeldOffBit) return "held off";
  switch (static_cast<StreamControl::State>(published)) {
    case StreamControl::State::Started: return "started";
    case StreamControl::State::Checking:
    case StreamControl::State::Starting: return "starting";
    case StreamControl::State::Suspending: return "suspending";
    case StreamControl::State::Idle: break;
  }
  return "suspended";
}

// A Bluetooth address as one word, so another task can read it without a lock.
// 0 = none (no device has the all-zero address).
uint64_t packBda(const uint8_t* bda) {
  uint64_t v = 0;
  for (int i = 0; i < ESP_BD_ADDR_LEN; ++i) v = (v << 8) | bda[i];
  return v;
}
}  // namespace

// ESP32-A2DP v1.8.11's source, with what a phone-like player needs and the
// library doesn't do: a media stream that starts on demand and isn't
// restarted against the headphones' will, reconnecting after every drop, AVRCP
// absolute volume without echoes or a second volume stage, and the AVRCP
// target for the headphones' buttons.
//
// The library's own media handling is bypassed: media-control acks, audio
// states and codec reports never reach its state machine (its connecting
// handler takes any of them as "connected", and its connected handler starts
// a stream on every heartbeat). BtControl (the stream and the volume) and
// ReconnectPlanner (lib/core, host-tested) make the decisions; this class
// only wires them to the stack and carries them out (BtControl::Io).
//
// Every handler runs on the library's app task (BtAppT), which drains its
// event queue, except app_gap_callback and app_rc_tg_callback (Bluedroid's BTC
// task). The loop task never touches this state: it posts work to BtAppT
// (requestStream, requestVolume*, requestTick, requestSuspendNow, requestForget).
//
// Bluedroid's BTC task (BTC_TASK) also runs the data callback (BtSink::onData):
// ESP-IDF 5.5's A2DP source has no media task of its own
// (btc_a2dp_source_startup(): btc_aa_src_task_hdl = btc_get_current_thread(),
// and its media timer posts there). So audio shares that task with
// app_gap_callback, app_rc_tg_callback, BtSink::onDeviceFound and the
// library's app_a2d_callback and app_rc_ct_callback. Those hand their events
// to BtAppT through bt_app_work_dispatch(): a malloc, then up to 10 ms of
// waiting for room in BtAppT's queue (BluetoothA2DPCommon.cpp:547; 20
// entries). BtAppT must keep its queue drained (no long work in its handlers,
// the loop's requests only a few at a time), or every such event can block
// BTC_TASK, and with it the audio, for 10 ms.
//
// Library internals relied on (ESP32-A2DP v1.8.11, pinned in platformio.ini;
// re-check on any upgrade): BluetoothA2DPSource.cpp's file-local
// BT_APP_HEART_BEAT_EVT 0xff00 (:21) and BT_APP_EVT_STACK_UP 0 (:24-26); the
// protected members s_a2d_state, s_connecting_heatbeat_count, discovery_active,
// peer_bd_addr, last_connection, reconnect_status, reconnect_retries,
// max_reconnect_retries, is_autoreconnect_allowed, last_heart_beat,
// app_task_handle (BluetoothA2DPSource.h:279-305, BluetoothA2DPCommon.h:373-416)
// and discoverability (BluetoothA2DPCommon.h:464); ccall_av_hdl_avrc_tg_evt.
class PlayerA2dp : public BluetoothA2DPSource, private BtControl::Io {
public:
  enum Work : uint16_t { kWantOff, kWantOn, kTick, kSuspendNow, kForget };
  static constexpr uint16_t kVolumeStep = 0x100;  // onVolumeWork: low byte is a signed step

  PlayerA2dp() {
    // Connectable while unconnected (see av_hdl_stack_evt), never discoverable:
    // the Core2 is a source and shouldn't show up in phones' device lists.
    // Set directly: set_discoverability() would call into a stack not yet up.
    discoverability = ESP_BT_NON_DISCOVERABLE;
  }

  // ---- loop task: work for BtAppT. false when its queue was full. ----
  bool requestStream(bool want) { return dispatch(onWork, want ? kWantOn : kWantOff); }
  bool requestTick() { return dispatch(onWork, kTick); }
  bool requestSuspendNow() { return dispatch(onWork, kSuspendNow); }
  bool requestForget() { return dispatch(onWork, kForget); }
  bool requestVolumeSet(uint8_t percent) { return dispatch(onVolumeWork, percent); }
  bool requestVolumeStep(int delta) {
    return dispatch(onVolumeWork, kVolumeStep | static_cast<uint8_t>(static_cast<int8_t>(delta)));
  }
  bool requestHeadroom(uint8_t db) { return dispatch(onHeadroomWork, db); }
  // BtAppT has finished starting the stack (its first 10 s go to that).
  bool ready() const { return ready_.load(); }

  // Before start(), while BtAppT and the data callback don't run yet.
  void initVolume() {
    sink->gain_.reset(ctl_.volume().gainQ15());
    publish();
  }

  TaskHandle_t appTask() const { return app_task_handle; }

  // Loop task, only when BtAppT couldn't do forgetPeer() and a restart
  // follows (which clears RAM): the NVS copy alone.
  void eraseStoredPeer() {
    esp_bd_addr_t none = {0, 0, 0, 0, 0, 0};
    write_address(last_bda_nvs_name(), none);
  }

  // The library's volume both scales the PCM and sends AVRCP absolute volume,
  // and returns early on an unchanged value (BluetoothA2DPCommon.h:232-246).
  // BtSink does both itself; nothing may call this.
  void set_volume(uint8_t) override {}

protected:
  // Data callback, on Bluedroid's BTC task (see above). No library volume
  // stage: BtSink::onData applies ours. (The library would run
  // its A2DPVolumeControl, BluetoothA2DPSource.cpp:188-192.)
  int32_t get_audio_data_volume(uint8_t* data, int32_t len) override { return get_audio_data(data, len); }

  // connect_to() calls this before paging (BluetoothA2DPCommon.cpp:100); the
  // library makes us non-connectable there (BluetoothA2DPSource.h:374-376).
  void set_scan_mode_connectable_default() override { set_scan_mode_connectable(!linkUp_.load()); }

  void bt_app_av_sm_hdlr(uint16_t event, void* param) override;
  void bt_av_hdl_avrc_ct_evt(uint16_t event, void* param) override;
  void av_hdl_stack_evt(uint16_t event, void* param) override;
  void av_hdl_avrc_tg_evt(uint16_t event, void* param) override;
  // BTC task.
  void app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) override;
  void app_rc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) override;

private:
  // File-local in BluetoothA2DPSource.cpp, mirrored here (v1.8.11).
  static constexpr uint16_t kStackUpEvt = 0;         // BT_APP_EVT_STACK_UP (:24-26)
  static constexpr uint16_t kHeartBeatEvt = 0xff00;  // BT_APP_HEART_BEAT_EVT (:21)
  // Our transaction labels for SET_ABSOLUTE_VOLUME. The library uses 0-4
  // (BluetoothA2DPCommon.h:149-153, BluetoothA2DPSource.cpp:20, :1003).
  static constexpr uint8_t kFirstLabel = 8;
  static constexpr uint8_t kLastLabel = 15;

  bool dispatch(bt_app_cb_t cb, uint16_t event) { return bt_app_work_dispatch(cb, event, nullptr, 0, nullptr); }
  static void onWork(uint16_t work, void*);
  static void onVolumeWork(uint16_t work, void*);
  static void onHeadroomWork(uint16_t db, void*);

  bool isLinked(const uint8_t* bda) const { return linkUp_ && packBda(bda) == linkedBda_.load(); }
  bool acceptable(const uint8_t* bda) const;
  void beginLinkUp(const uint8_t* bda);
  void finishLinkUp(const esp_a2d_cb_param_t& a2d);
  void linkDown(const esp_a2d_cb_param_t& a2d);
  void heartbeat();
  void rearmReconnect(const uint8_t* bda);
  void forgetPeer();
  void logCodec(const esp_a2d_mcc_t& mcc);

  // BtControl::Io (BtAppT)
  bool mediaCtrl(StreamControl::Cmd cmd) override;
  void sendAbsoluteVolume(uint8_t absolute) override;
  void setGain(uint16_t q15, BtControl::GainMove move) override;
  void volumeChanged() override;
  void volumeModeChanged(AbsVolumePolicy::Mode mode, bool probeUnanswered) override;
  void remoteSuspend() override;
  void commandGaveUp() override;
  void streamStarted(uint32_t afterMs) override;
  void publish() override;

  // Also read on the BTC task.
  std::atomic<bool> linkUp_{false};
  std::atomic<uint64_t> linkedBda_{0};  // packBda() of the linked device, 0 while unlinked
  std::atomic<bool> ready_{false};

  // BtAppT only from here on.
  BtControl ctl_{*this, BtSink::kDefaultVolume};
  ReconnectPlanner reconnect_;
  uint8_t label_ = kFirstLabel;
  bool codecLogged_ = false;
  bool delayLogged_ = false;
  bool lateProbe_ = false;  // the last probe logged was a late one (its timeout)
};

namespace {
PlayerA2dp a2dp;
}  // namespace

// ---- PlayerA2dp: work posted by the loop task ----

void PlayerA2dp::onWork(uint16_t work, void*) {
  const uint32_t now = millis();
  switch (work) {
    case kWantOn:
    case kWantOff:
      a2dp.ctl_.setWanted(work == kWantOn, now);
      break;
    case kTick:
      a2dp.ctl_.tick(now);
      break;
    case kSuspendNow:
      a2dp.ctl_.suspendNow(now);
      break;
    case kForget:
      a2dp.forgetPeer();
      break;
  }
}

void PlayerA2dp::onVolumeWork(uint16_t work, void*) {
  if (work & kVolumeStep) {
    a2dp.ctl_.stepVolume(static_cast<int8_t>(work & 0xFF), millis());
  } else {
    a2dp.ctl_.setVolume(static_cast<uint8_t>(std::min<int>(100, work & 0xFF)), millis());
  }
}

// A diagnostic: our fixed attenuation, -db dB.
void PlayerA2dp::onHeadroomWork(uint16_t db, void*) {
  a2dp.ctl_.setHeadroom(vol::dbToQ15(-static_cast<float>(db)), millis());
}

// Forgets the remembered headphones in NVS and RAM. On BtAppT, which also
// pages, accepts and re-arms with last_connection: no torn address, and a
// link coming up can't write the old one back afterwards.
// clean_last_connection() would skip the NVS write once the library's
// reconnect retries ran out (BluetoothA2DPCommon.cpp:278-281).
void PlayerA2dp::forgetPeer() {
  esp_bd_addr_t none = {0, 0, 0, 0, 0, 0};
  write_address(last_bda_nvs_name(), none);
  std::memset(last_connection, 0, ESP_BD_ADDR_LEN);
  if (!linkUp_) std::memset(peer_bd_addr, 0, ESP_BD_ADDR_LEN);
  // Nothing to page any more (the library would page the zero address).
  reconnect_status = NoReconnect;
  is_autoreconnect_allowed = false;
  Serial.println("[bt] forgot the remembered headphones");
  sink->forgotten_.store(true, std::memory_order_release);
}

// ---- PlayerA2dp: A2DP ----

void PlayerA2dp::bt_app_av_sm_hdlr(uint16_t event, void* param) {
  auto* a2d = static_cast<esp_a2d_cb_param_t*>(param);
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
      auto& c = a2d->conn_stat;
      if (!acceptable(c.remote_bda)) {
        // Something other than the remembered headphones connecting in. The
        // library would take it as ours: in DISCOVERING it ignores the event
        // but still reports it (BluetoothA2DPSource.cpp:628-634).
        if (c.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
          Serial.printf("[bt] refused %s: not the remembered headphones\n", to_str(c.remote_bda));
          esp_a2d_source_disconnect(c.remote_bda);
        }
        return;
      }
      if (c.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        if (linkUp_) return;  // a repeated report of the link that is up
        beginLinkUp(c.remote_bda);
        BluetoothA2DPSource::bt_app_av_sm_hdlr(event, param);  // callbacks (the loop hears of it); state machine
        finishLinkUp(*a2d);
      } else {
        BluetoothA2DPSource::bt_app_av_sm_hdlr(event, param);
        if (c.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) linkDown(*a2d);
      }
      return;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
      if (!isLinked(a2d->audio_stat.remote_bda)) return;
      const bool started = a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
      ctl_.audioState(started, millis());
      if (started && !codecLogged_) {
        Serial.println("[bt] codec: not reported by the stack for this connection");
        codecLogged_ = true;
      }
      return;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
      // Arrives while the link is still opening, before CONNECTED. The library
      // would take it as "connected" and send CHECK_SRC_RDY (:742-749).
      if (acceptable(a2d->audio_cfg.remote_bda)) logCodec(a2d->audio_cfg.mcc);
      return;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
      if (!delayLogged_) {
        delayLogged_ = true;
        const unsigned d = a2d->a2d_report_delay_value_stat.delay_value;  // 1/10 ms
        Serial.printf("[bt] headphones report %u.%u ms of delay\n", d / 10, d % 10);
      }
      return;
#endif
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
      // Before the link is up these answer the library's connecting-state
      // probes (:748, :752); StreamControl ignores them then.
      StreamControl::Cmd cmd;
      switch (a2d->media_ctrl_stat.cmd) {
        case ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY: cmd = StreamControl::Cmd::CheckReady; break;
        case ESP_A2D_MEDIA_CTRL_START: cmd = StreamControl::Cmd::Start; break;
        case ESP_A2D_MEDIA_CTRL_SUSPEND: cmd = StreamControl::Cmd::Suspend; break;
        default: return;
      }
      ctl_.mediaAck(cmd, a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS, millis());
      return;
    }
    case kHeartBeatEvt:
      heartbeat();
      return;
    default:
      break;
  }
  BluetoothA2DPSource::bt_app_av_sm_hdlr(event, param);
}

// The linked headphones; while unlinked, the remembered ones, or the device
// we are connecting to ourselves (picked during discovery).
bool PlayerA2dp::acceptable(const uint8_t* bda) const {
  if (linkUp_) return isLinked(bda);
  return std::memcmp(bda, last_connection, ESP_BD_ADDR_LEN) == 0 ||
         std::memcmp(bda, peer_bd_addr, ESP_BD_ADDR_LEN) == 0;
}

// Before the library's callbacks tell the loop: the BTC task's filters must
// know the link, and the gain must be at the safe level (snapped) before any
// media can flow, even a stream the headphones start themselves.
void PlayerA2dp::beginLinkUp(const uint8_t* bda) {
  linkedBda_ = packBda(bda);
  linkUp_ = true;
  sink->streamEpoch_.fetch_add(1, std::memory_order_release);  // its first audio fades in
  ctl_.linkUp(millis());
}

void PlayerA2dp::finishLinkUp(const esp_a2d_cb_param_t& a2d) {
  uint8_t bda[ESP_BD_ADDR_LEN];
  std::memcpy(bda, a2d.conn_stat.remote_bda, ESP_BD_ADDR_LEN);
  // Also for connections the library's state machine ignores: made by the
  // headphones, or while it was discovering (BluetoothA2DPSource.cpp:632-634).
  s_a2d_state = APP_AV_STATE_CONNECTED;
  s_connecting_heatbeat_count = 0;
  std::memcpy(peer_bd_addr, bda, ESP_BD_ADDR_LEN);
  rearmReconnect(bda);
  // Unconditionally: discovery_active is only set once the stack reports the
  // start, which may still be on its way. Harmless when nothing runs.
  esp_bt_gap_cancel_discovery();
  set_scan_mode_connectable(false);
  esp_bt_gap_read_remote_name(bda);  // for the UI after an automatic reconnect
  Serial.printf("[bt] A2DP up (%s), audio MTU %u\n", to_str(bda), static_cast<unsigned>(a2d.conn_stat.audio_mtu));
}

void PlayerA2dp::linkDown(const esp_a2d_cb_param_t& a2d) {
  if (!linkUp_.exchange(false)) return;
  linkedBda_ = 0;
  s_a2d_state = APP_AV_STATE_UNCONNECTED;  // the heartbeat's reconnect logic takes over
  codecLogged_ = false;
  delayLogged_ = false;
  Serial.printf("[bt] A2DP down (%s)\n",
                a2d.conn_stat.disc_rsn == ESP_A2D_DISC_RSN_ABNORMAL ? "signal lost" : "closed");
  ctl_.linkDown(millis());
  set_scan_mode_connectable(true);  // the headphones may come back by themselves
}

// The library's 10 s heartbeat: ReconnectPlanner decides, this carries it out.
void PlayerA2dp::heartbeat() {
  // Unlinked it can't be connected (e.g. a link the library never saw close).
  if (!linkUp_ && s_a2d_state == APP_AV_STATE_CONNECTED) s_a2d_state = APP_AV_STATE_UNCONNECTED;
  ReconnectPlanner::Lib state = ReconnectPlanner::Lib::Other;
  switch (s_a2d_state) {
    case APP_AV_STATE_UNCONNECTED: state = ReconnectPlanner::Lib::Unconnected; break;
    case APP_AV_STATE_CONNECTING: state = ReconnectPlanner::Lib::Connecting; break;
    case APP_AV_STATE_DISCOVERING: state = ReconnectPlanner::Lib::Discovering; break;
    default: break;
  }
  const ReconnectPlanner::In in{linkUp_.load(),
                                state,
                                discovery_active,
                                is_autoreconnect_allowed && reconnect_status == AutoReconnect,
                                has_last_connection(),
                                static_cast<uint32_t>(get_millis())};
  switch (reconnect_.heartbeat(in)) {
    case ReconnectPlanner::Do::KeepAlive:
      // Connected, the library would start an idle stream (BluetoothA2DPSource.cpp:807-812).
      last_heart_beat = get_millis();
      return;
    case ReconnectPlanner::Do::KeepScanning:
      return;
    case ReconnectPlanner::Do::StopScanAndPage:
      Serial.printf("[bt] nothing found by scanning: trying %s again\n", to_str(last_connection));
      s_a2d_state = APP_AV_STATE_UNCONNECTED;  // app_gap_callback won't restart discovery now
      esp_bt_gap_cancel_discovery();
      reconnect_status = AutoReconnect;
      reconnect_retries = max_reconnect_retries;
      is_autoreconnect_allowed = true;
      break;
    case ReconnectPlanner::Do::ReArm:
      Serial.printf("[bt] trying %s again\n", to_str(last_connection));
      reconnect_status = AutoReconnect;
      reconnect_retries = max_reconnect_retries;
      is_autoreconnect_allowed = true;
      break;
    case ReconnectPlanner::Do::Scan:
      Serial.println("[bt] scanning for headphones again");
      s_a2d_state = APP_AV_STATE_DISCOVERING;
      esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
      return;
    case ReconnectPlanner::Do::PassOn:
      break;
  }
  // Unconnected: retries, then discovery (:675-683, :700-718). Connecting: the
  // library's 2-heartbeat timeout (:750-757).
  BluetoothA2DPSource::bt_app_av_sm_hdlr(kHeartBeatEvt, nullptr);
}

// ESP32-A2DP leaves auto-reconnect off after a connection made through
// discovery (start() only enables it when NVS had an address,
// BluetoothA2DPSource.cpp:99-111) and for good once its retries ran out
// (:710-716), so a later drop never reconnected. Re-armed on every link, and
// NVS is checked directly: set_last_connection() skips the write when RAM
// already holds the address or while retries are exhausted
// (BluetoothA2DPCommon.cpp:274-281).
void PlayerA2dp::rearmReconnect(const uint8_t* bda) {
  reconnect_status = AutoReconnect;
  reconnect_retries = max_reconnect_retries;
  is_autoreconnect_allowed = true;
  esp_bd_addr_t addr;
  std::memcpy(addr, bda, ESP_BD_ADDR_LEN);
  std::memcpy(last_connection, addr, ESP_BD_ADDR_LEN);
  esp_bd_addr_t stored;
  if (!read_address(last_bda_nvs_name(), stored) || std::memcmp(stored, addr, ESP_BD_ADDR_LEN) != 0) {
    write_address(last_bda_nvs_name(), addr);
  }
}

void PlayerA2dp::logCodec(const esp_a2d_mcc_t& mcc) {
  codecLogged_ = true;
  if (mcc.type != ESP_A2D_MCT_SBC) {
    Serial.printf("[bt] codec type 0x%02x\n", static_cast<unsigned>(mcc.type));
    return;
  }
  esp_a2d_cie_sbc_t c;  // packed bitfields: copy out
  std::memcpy(&c, &mcc.cie.sbc_info, sizeof(c));
  Serial.printf("[bt] codec: SBC %s, %s, %s blocks, %s subbands, %s allocation, bitpool %u-%u\n",
                bitName(c.samp_freq, "16 kHz", "32 kHz", "44.1 kHz", "48 kHz"),
                bitName(c.ch_mode, "mono", "dual channel", "stereo", "joint stereo"),
                bitName(c.block_len, "4", "8", "12", "16"), bitName(c.num_subbands, "?", "?", "4", "8"),
                bitName(c.alloc_mthd, "?", "?", "SNR", "loudness"), static_cast<unsigned>(c.min_bitpool),
                static_cast<unsigned>(c.max_bitpool));
}

// ---- PlayerA2dp: BtControl::Io ----

// Only BtControl issues media control, on BtAppT, so there is never more than
// one command outstanding (ESP-IDF answers any other BUSY,
// btc_a2dp_control.c:101-104).
bool PlayerA2dp::mediaCtrl(StreamControl::Cmd cmd) {
  esp_a2d_media_ctrl_t c = ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY;  // then START once it's acknowledged
  if (cmd == StreamControl::Cmd::Start) {
    c = ESP_A2D_MEDIA_CTRL_START;
    // Before the stream can start: the data callback fades its audio in.
    sink->streamEpoch_.fetch_add(1, std::memory_order_release);
  }
  // Our own suspend, not the library's STOPPING state: that disconnects (:906-915).
  if (cmd == StreamControl::Cmd::Suspend) c = ESP_A2D_MEDIA_CTRL_SUSPEND;
  return esp_a2d_media_ctrl(c) == ESP_OK;
}

void PlayerA2dp::sendAbsoluteVolume(uint8_t absolute) {
  // Queued to the BTC task. Fails silently if the headphones lack the
  // metadata feature (btc_avrc.c:1378-1388); the policy's timeout covers it.
  esp_avrc_ct_send_set_absolute_volume_cmd(label_, absolute);
  label_ = label_ == kLastLabel ? kFirstLabel : label_ + 1;
}

void PlayerA2dp::setGain(uint16_t q15, BtControl::GainMove move) {
  switch (move) {  // lock-free hand-over to the data callback
    case BtControl::GainMove::Ramp: sink->gain_.request(q15, false); break;
    case BtControl::GainMove::Snap: sink->gain_.request(q15, true); break;
    case BtControl::GainMove::Lift: sink->gain_.lift(q15); break;
  }
}

void PlayerA2dp::volumeChanged() { sink->post(BtSink::Event::VolumeChanged); }

void PlayerA2dp::volumeModeChanged(AbsVolumePolicy::Mode mode, bool probeUnanswered) {
  const AbsVolumePolicy& v = ctl_.volume();
  const unsigned percent = v.percent();
  switch (mode) {
    case AbsVolumePolicy::Mode::Probing:
      lateProbe_ = v.ducked();
      if (lateProbe_) {
        Serial.printf("[bt] volume: the headphones' remote control came up after playback started: dipping, "
                      "then handing them the volume (%u%%)\n",
                      percent);
      } else {
        Serial.printf("[bt] volume: asking the headphones to take it over (AVRCP absolute volume, %u%%)\n", percent);
      }
      break;
    case AbsVolumePolicy::Mode::Absolute:
      Serial.printf("[bt] volume: the headphones control it (%u%%); the Core2 only keeps %.1f dB of headroom\n",
                    percent, static_cast<double>(vol::q15ToDb(v.headroomQ15())));
      break;
    case AbsVolumePolicy::Mode::Software:
      if (probeUnanswered) {
        Serial.printf("[bt] volume: asked the headphones, no answer in %lu ms; applied by the Core2 "
                      "(software, %u%%); steps down still sent to them\n",
                      static_cast<unsigned long>(lateProbe_ ? AbsVolumePolicy::kLateProbeTimeoutMs
                                                            : AbsVolumePolicy::kProbeTimeoutMs),
                      percent);
      } else {
        Serial.printf("[bt] volume: applied by the Core2 (software, %u%%)\n", percent);
      }
      break;
  }
}

void PlayerA2dp::remoteSuspend() {
  // Out of the ear, another source, ...: don't fight it. Pause; the next
  // play starts it again.
  Serial.println("[bt] the headphones stopped the stream: pausing");
  sink->post(BtSink::Event::Suspended);
}

void PlayerA2dp::commandGaveUp() { Serial.println("[bt] a media command got no (real) answer in 3 s: gave up on it"); }

void PlayerA2dp::streamStarted(uint32_t afterMs) {
  Serial.printf("[bt] audio stream started %lu ms after playback asked for it\n", static_cast<unsigned long>(afterMs));
}

// What the loop task and the stats see.
void PlayerA2dp::publish() {
  const AbsVolumePolicy& v = ctl_.volume();
  sink->volume_.store(v.percent());
  sink->headroom_.store(v.headroomQ15());
  sink->control_.store(static_cast<uint8_t>(v.mode()));
  sink->headsetVolume_.store(static_cast<int16_t>(v.headsetAbsolute()));
  const StreamControl& s = ctl_.stream();
  sink->streaming_ = s.streaming();
  sink->streamState_ = static_cast<uint8_t>(s.state()) | (s.heldOff() ? kHeldOffBit : 0);
}

// ---- PlayerA2dp: AVRCP controller ----

void PlayerA2dp::bt_av_hdl_avrc_ct_evt(uint16_t event, void* param) {
  auto* rc = static_cast<esp_avrc_ct_cb_param_t*>(param);
  const uint32_t now = millis();
  switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
      // Library: asks for the notification capabilities (or forgets them).
      // With avrc_rn_events empty (begin()) it registers nothing beforehand.
      BluetoothA2DPSource::bt_av_hdl_avrc_ct_evt(event, param);
      if (rc->conn_stat.connected) {
        ctl_.avrcpUp(now);
      } else {
        ctl_.avrcpDown(now);
      }
      return;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {  // the library only logs it
      const uint32_t f = rc->rmt_feats.feat_mask;
      // ESP-IDF sends SET_ABSOLUTE_VOLUME only to targets with the metadata
      // feature (btc_avrc.c:1378-1388, BTA_AV_FEAT_METADATA).
      Serial.printf("[bt] AVRCP: headphones' features 0x%lx, target flags 0x%x (metadata, needed for absolute "
                    "volume: %s; advanced control: %s; category 2: %s)\n",
                    static_cast<unsigned long>(f), static_cast<unsigned>(rc->rmt_feats.tg_feat_flag),
                    (f & ESP_AVRC_FEAT_META_DATA) ? "yes" : "no", (f & ESP_AVRC_FEAT_ADV_CTRL) ? "yes" : "no",
                    (rc->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_CAT2) ? "yes" : "no");
      return;
    }
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
      // Library: keeps the capabilities and registers for VOLUME_CHANGE (label
      // 1) if listed (BluetoothA2DPSource.cpp:1065-1071, :956-965).
      BluetoothA2DPSource::bt_av_hdl_avrc_ct_evt(event, param);
      esp_avrc_rn_evt_cap_mask_t caps = rc->get_rn_caps_rsp.evt_set;
      const bool volumeEvents =
          esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps, ESP_AVRC_RN_VOLUME_CHANGE);
      Serial.printf("[bt] AVRCP: headphones' notifications 0x%04x: absolute volume %s\n",
                    static_cast<unsigned>(caps.bits), volumeEvents ? "offered" : "not offered");
      // Like a phone: the Bluetooth volume becomes the headphones' volume
      // (AbsVolumePolicy; after a dip to silence if audio has played on this
      // link already). Their current one can't be known: ESP-IDF drops the
      // INTERIM reply to the registration (btc_avrc.c:961-965).
      ctl_.capabilities(volumeEvents, now);
      return;
    }
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
      // ESP-IDF raises it only when accepted (btc_avrc.c:975-978). Not passed
      // on: the library would call set_volume() (BluetoothA2DPSource.cpp:1074-1078).
      ctl_.accepted(rc->set_volume_rsp.volume & 0x7F, now);
      return;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
      if (rc->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        // Their own buttons. Not passed on: the library echoes the volume
        // back and scales the PCM with it (BluetoothA2DPSource.cpp:967-980).
        bt_av_volume_changed();  // a notification fires once: register again
        ctl_.headsetChanged(rc->change_ntf.event_parameter.volume & 0x7F, now);
        return;
      }
      break;
    default:
      break;
  }
  BluetoothA2DPSource::bt_av_hdl_avrc_ct_evt(event, param);  // e.g. frees metadata text
}

// ---- PlayerA2dp: AVRCP target (the headphones' buttons) ----

// BTC task. The library's version copies sizeof(esp_a2d_cb_param_t) out of an
// esp_avrc_tg_cb_param_t (BluetoothA2DPSource.cpp:1109-1110), reading past it.
void PlayerA2dp::app_rc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) {
  switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    case ESP_AVRC_TG_SET_PLAYER_APP_VALUE_EVT:
      bt_app_work_dispatch(ccall_av_hdl_avrc_tg_evt, event, param, sizeof(esp_avrc_tg_cb_param_t), nullptr);
      break;
    default:
      break;
  }
}

void PlayerA2dp::av_hdl_avrc_tg_evt(uint16_t event, void* param) {
  auto* rc = static_cast<esp_avrc_tg_cb_param_t*>(param);
  switch (event) {
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
      Serial.printf("[bt] AVRCP: headphones' controller features 0x%lx, flags 0x%x\n",
                    static_cast<unsigned long>(rc->rmt_feats.feat_mask), static_cast<unsigned>(rc->rmt_feats.ct_feat_flag));
      return;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
      // Our target offers no notifications (begin()): ESP-IDF's target can
      // only offer VOLUME_CHANGE (btc_avrc.c cs_rn_allowed_evt), not
      // PLAY_STATUS_CHANGE, and rejects GET_PLAY_STATUS. The headphones pick
      // PLAY or PAUSE from the stream, which a pause from their key suspends
      // at once (BtSink::suspendPromptly()). Logged to learn what they want.
      Serial.printf("[bt] AVRCP: headphones asked for event %u notifications (not offered)\n",
                    static_cast<unsigned>(rc->reg_ntf.event_id));
      return;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
      Serial.printf("[bt] AVRCP: headphones sent absolute volume %u to the Core2 (ignored)\n",
                    static_cast<unsigned>(rc->set_abs_vol.volume));
      return;
    default:
      // Connection: the library marks every allowed key as supported.
      // Passthrough: the library calls BtSink::onKey (:1139-1146).
      BluetoothA2DPSource::av_hdl_avrc_tg_evt(event, param);
      return;
  }
}

// ---- PlayerA2dp: stack up, discovery ----

void PlayerA2dp::av_hdl_stack_evt(uint16_t event, void* param) {
  // Blocks BtAppT for 10 s, then reconnects or starts discovery, leaving us
  // non-connectable if it discovers (BluetoothA2DPSource.cpp:547-559).
  BluetoothA2DPSource::av_hdl_stack_evt(event, param);
  if (event != kStackUpEvt) return;
  // Stay connectable while unconnected, so the remembered headphones can
  // reconnect by themselves when they wake up, as they do to a phone.
  if (!linkUp_) set_scan_mode_connectable(true);
  ready_ = true;  // the loop's work can come in now
}

// BTC task. Reads s_a2d_state, which BtAppT writes (as the library itself does).
void PlayerA2dp::app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
      // Only while we are looking. Linked, a match would overwrite
      // peer_bd_addr and last_connection (NVS too) and park the library in
      // DISCOVERED, where it ignores every event (BluetoothA2DPSource.cpp:358-364, :631-634).
      if (linkUp_ || s_a2d_state != APP_AV_STATE_DISCOVERING) return;
      break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED && linkUp_) {
        esp_bt_gap_cancel_discovery();  // started just before the link came up
        break;
      }
      // The library starts another round whenever one ends without a match
      // (BluetoothA2DPSource.cpp:389-395): only right while we are looking.
      // Inquiry while streaming costs airtime, and while paging it competes.
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
          (linkUp_ || (s_a2d_state != APP_AV_STATE_DISCOVERING && s_a2d_state != APP_AV_STATE_DISCOVERED))) {
        discovery_active = false;
        return;
      }
      break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
      // The library reports any ACL drop as our link going down (:465-471),
      // also one to another device, e.g. one we refused.
      const uint8_t* bda = param->acl_disconn_cmpl_stat.bda;
      if (packBda(bda) != linkedBda_.load() && std::memcmp(bda, peer_bd_addr, ESP_BD_ADDR_LEN) != 0) return;
      break;
    }
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:  // asked for in finishLinkUp()
      if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
        sink->setDeviceName(reinterpret_cast<const char*>(param->read_rmt_name.rmt_name));
      }
      return;
    default:
      break;
  }
  BluetoothA2DPSource::app_gap_callback(event, param);
}

// ---- BtSink ----

void BtSink::begin(PcmRing& ring, AudioShared& shared, const char* defaultSinkName) {
  ring_ = &ring;
  shared_ = &shared;
  reader_.bind(ring);
  sink = this;
  events_ = xQueueCreate(kEventQueueLength, sizeof(Event));

  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  const String saved = prefs.getString(kPrefsSinkName, defaultSinkName ? defaultSinkName : "");
  prefs.end();
  strlcpy(sinkNames_[0], saved.c_str(), sizeof(sinkNames_[0]));

  a2dp.initVolume();
  a2dp.set_local_name("mStream Player");
  a2dp.set_ssid_callback(onDeviceFound);
  a2dp.set_on_connection_state_changed(onConnectionChanged);
  a2dp.set_data_callback_in_frames(onData);
  a2dp.set_auto_reconnect(true, kReconnectTries);  // off by default in ESP32-A2DP
  // Headphone buttons. The AVRCP target is only set up when this is set before
  // start() (BluetoothA2DPSource.cpp:522-526).
  a2dp.set_avrc_passthru_command_callback(onKey);
  // Nothing registered before the capabilities are known, and our target
  // offers no notifications (default: VOLUME_CHANGE, which it never answers;
  // BluetoothA2DPSource.cpp:528-536, :1005-1013, :1154-1169).
  a2dp.set_avrc_rn_events({});
  // BtAppT (the library's event task) next to Bluedroid on core 0, off the
  // decode/UI core (default: core 1, BluetoothA2DPCommon.h:407).
  a2dp.set_task_core(0);
  a2dp.set_event_stack_size(kAppTaskStack);
  a2dp.start();
}

bool BtSink::connected() const { return a2dp.is_connected(); }

bool BtSink::headphonesControlVolume() const {
  return static_cast<AbsVolumePolicy::Mode>(control_.load()) == AbsVolumePolicy::Mode::Absolute;
}

void BtSink::setVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  unsentStep_ = 0;  // superseded
  unsentVolume_ = a2dp.ready() && a2dp.requestVolumeSet(percent) ? -1 : percent;
}

void BtSink::stepVolume(int delta) {
  delta = std::min(100, std::max(-100, delta));
  if (unsentVolume_ < 0 && unsentStep_ == 0 && a2dp.ready() && a2dp.requestVolumeStep(delta)) return;
  unsentStep_ = std::min(100, std::max(-100, unsentStep_ + delta));  // sent with the next update()
}

void BtSink::setHeadroomDb(uint8_t db) {
  unsentHeadroom_ = a2dp.ready() && a2dp.requestHeadroom(db) ? -1 : db;
}

void BtSink::suspendPromptly() {
  suspendPending_ = true;
  suspendPendingMs_ = millis();
}

void BtSink::update(uint32_t nowMs, bool wantAudio) {
  // BtAppT spends its first 10 s starting the stack; nothing is lost meanwhile.
  if (!a2dp.ready()) return;
  // Each is retried on the next pass when BtAppT's queue has no room.
  if (forgetPending_ && a2dp.requestForget()) forgetPending_ = false;
  if (unsentVolume_ >= 0 && a2dp.requestVolumeSet(static_cast<uint8_t>(unsentVolume_))) unsentVolume_ = -1;
  if (unsentVolume_ < 0 && unsentStep_ != 0 && a2dp.requestVolumeStep(unsentStep_)) unsentStep_ = 0;
  if (unsentHeadroom_ >= 0 && a2dp.requestHeadroom(static_cast<uint8_t>(unsentHeadroom_))) unsentHeadroom_ = -1;
  if (wantAudio != wantAudio_ && a2dp.requestStream(wantAudio)) wantAudio_ = wantAudio;
  if (suspendPending_) {
    // Once the pause has reached BtAppT (queued in order after it).
    if (!wantAudio_) {
      if (a2dp.requestSuspendNow()) suspendPending_ = false;
    } else if (nowMs - suspendPendingMs_ >= kSuspendPromptlyForMs) {
      suspendPending_ = false;  // still playing: nothing to suspend
    }
  }
  if (static_cast<int32_t>(nowMs - nextTickMs_) >= 0) {
    a2dp.requestTick();  // a lost tick is just a late one
    nextTickMs_ = nowMs + kTickMs;
  }
}

bool BtSink::forgetDevice(uint32_t waitMs) {
  forgotten_.store(false, std::memory_order_relaxed);
  forgetPending_ = true;
  const uint32_t start = millis();
  for (;;) {
    if (forgetPending_ && a2dp.ready() && a2dp.requestForget()) forgetPending_ = false;
    if (forgotten_.load(std::memory_order_acquire)) return true;
    if (millis() - start >= waitMs) break;
    delay(10);
  }
  // BtAppT is still starting the stack (its first 10 s) or busy. Waited for:
  // a restart follows, so erasing the NVS copy here is enough; otherwise
  // update() posts it later.
  if (waitMs > 0) {
    a2dp.eraseStoredPeer();
    return true;
  }
  return false;
}

void BtSink::setSinkName(const char* name) {
  const uint8_t next = sinkNameIdx_.load() ^ 1;
  strlcpy(sinkNames_[next], name, sizeof(sinkNames_[next]));
  sinkNameIdx_.store(next);
  lastLogged_[0] = '\0';  // report devices again under the new rule

  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putString(kPrefsSinkName, sinkNames_[next]);
  prefs.end();
}

BtSink::Event BtSink::takeEvent() {
  Event e = Event::None;
  if (events_ && xQueueReceive(events_, &e, 0) == pdTRUE) return e;
  return Event::None;
}

BtSink::Stats BtSink::stats() {
  Stats s;
  s.volume = volume();
  s.volumeControl = controlName(static_cast<AbsVolumePolicy::Mode>(control_.load()));
  s.headsetVolume = headsetVolume_.load();
  s.gainQ15 = gain_.currentQ15();
  s.headroomQ15 = headroom_.load();
  s.stream = streamName(streamState_.load());
  s.maxGapMs = maxGapUs_.exchange(0, std::memory_order_relaxed) / 1000;
  s.eventsDropped = eventsDropped_.load(std::memory_order_relaxed);
  const TaskHandle_t task = a2dp.appTask();
  s.appTaskStackFree = task ? uxTaskGetStackHighWaterMark(task) : 0;
  return s;
}

void BtSink::post(Event e) {
  if (!events_ || xQueueSend(events_, &e, 0) != pdTRUE) eventsDropped_.fetch_add(1, std::memory_order_relaxed);
}

void BtSink::setDeviceName(const char* name) {
  if (!name[0]) return;
  const uint8_t next = deviceNameIdx_.load() ^ 1;
  strlcpy(deviceNames_[next], name, sizeof(deviceNames_[next]));
  deviceNameIdx_.store(next);
}

// Bluetooth data callback, on Bluedroid's BTC task (see the task note above
// PlayerA2dp), 128 frames at a time at 44.1 kHz, several times per ~30 ms
// tick. No blocking, no logging.
int32_t BtSink::onData(Frame* frames, int32_t count) {
  BtSink& s = *sink;
  // A new or resumed stream (StreamRestart): the listener heard silence, so
  // the audio fades in from 0 (the fader) back to the level heard before, or
  // the level a handover lifted it to (the gain stage). Detected here, on the
  // task that owns both. ESP-IDF's flush when the stream stops
  // (btc_a2dp_source_aa_tx_flush: data NULL, len -1) arrives here as 0
  // frames, on this same task.
  const bool restart =
      s.restart_.callback(esp_timer_get_time(), count, s.streamEpoch_.load(std::memory_order_acquire));
  if (count <= 0) return 0;
  if (restart) {
    s.gain_.restart();
    s.reader_.reset();
  }
  if (s.restart_.statGapUs() > s.maxGapUs_.load(std::memory_order_relaxed)) {
    s.maxGapUs_.store(s.restart_.statGapUs(), std::memory_order_relaxed);
  }

  auto* out = reinterpret_cast<int16_t*>(frames);
  const uint32_t want = static_cast<uint32_t>(count);
  const bool playing = !s.shared_->paused.load(std::memory_order_relaxed);
  // The only ring access. Reads only while Bluetooth is the ring's consumer
  // (one consumer at a time) and only what the fader wants: paused, the next
  // 64 frames fade out and the rest stay for resume. Always writes all `want`
  // frames: audio, then a fade to 0 if it ran short, then zeros.
  const DeclickReader::Result r = s.reader_.fill(out, want, playing);
  // A gap in audio that should be there. Not while paused (the fade-out may
  // find the ring empty) nor while the speaker has the ring (wanted is 0).
  if (playing && r.read < r.wanted && s.shared_->expectingAudio.load(std::memory_order_relaxed)) {
    s.shared_->underruns.fetch_add(1, std::memory_order_relaxed);
  }
  // Then the volume, over every frame so its ramps keep real time. Both
  // stages only ever multiply by at most 1: together they never add level.
  s.gain_.process(out, want);
  s.framesPulled_.fetch_add(want, std::memory_order_relaxed);
  return count;
}

// BTC task, during discovery. Only called for devices that advertise
// themselves as audio sinks.
bool BtSink::onDeviceFound(const char* name, esp_bd_addr_t, int rssi) {
  BtSink& s = *sink;
  const char* target = s.sinkName();
  const bool named = target[0] != '\0';
  const bool wanted = named ? containsIgnoreCase(name, target) : rssi >= kMinRssi;
  if (wanted) {
    s.setDeviceName(name);
    Serial.printf("[bt] connecting to \"%s\" (rssi %d)\n", name, rssi);
  } else if (std::strcmp(name, s.lastLogged_) != 0) {  // discovery repeats; log once
    Serial.printf("[bt] found \"%s\" (rssi %d), not using it: %s\n", name, rssi,
                  named ? "name doesn't match BT_SINK_NAME" : "not close enough, or set BT_SINK_NAME");
  }
  strlcpy(s.lastLogged_, name, sizeof(s.lastLogged_));
  return wanted;
}

// BtAppT; a DISCONNECTED also comes from Bluedroid's BTC task when the ACL of
// the linked (or paged) device drops (BluetoothA2DPSource.cpp:465-471).
void BtSink::onConnectionChanged(esp_a2d_connection_state_t state, void*) {
  BtSink& s = *sink;
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    if (s.linked_.exchange(true)) return;
    s.post(Event::Connected);
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    // Only a link that was up: a failed (re)connect attempt reports this too.
    if (!s.linked_.exchange(false)) return;
    s.streaming_ = false;
    s.post(Event::Disconnected);
  }
}

// BtAppT: a headphone button (AVRCP passthrough). Acts on the press, so a held
// volume key repeats; the release that follows is ignored.
void BtSink::onKey(uint8_t key, bool released) {
  if (released) return;
  Event e = Event::None;
  switch (key) {
    case ESP_AVRC_PT_CMD_PLAY: e = Event::Play; break;
    case ESP_AVRC_PT_CMD_PAUSE:
    case ESP_AVRC_PT_CMD_STOP: e = Event::Pause; break;
    case ESP_AVRC_PT_CMD_FORWARD: e = Event::Next; break;
    case ESP_AVRC_PT_CMD_BACKWARD: e = Event::Prev; break;
    case ESP_AVRC_PT_CMD_VOL_UP: e = Event::VolumeUp; break;
    case ESP_AVRC_PT_CMD_VOL_DOWN: e = Event::VolumeDown; break;
    default: break;
  }
  Serial.printf("[bt] headphone key 0x%02x%s\n", key, e == Event::None ? " (not used)" : "");
  if (e != Event::None) sink->post(e);
}
