#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "esphome/components/rs485_bus/rs485_bus.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "nibe_frame.h"
#include "nibe_queries.h"
#include "nibe_wire.h"

#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_ESP32
#include "esp_timer.h"
#endif
#ifdef USE_NIBE_DISPLAY_WEB
#include "esphome/components/socket/socket.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome::nibe {

class NibeBus;  // defined below; NibeButton references it

#ifdef USE_NUMBER
/// Front-panel knob number: the value is the PLACEMENT (u8 0-255); control() queues the
/// cmd-0x40 knob event [KEY_KNOB, placement, 0, 0] via queue_key_number() (docs/commands.md).
class NibeKnobNumber : public number::Number, public Component {
 public:
  void setup() override;
  void control(float value) override;
  void set_hub(NibeBus *hub) { this->hub_ = hub; }

 protected:
  NibeBus *hub_{nullptr};
};

/// Register-address number: pins the 0x72 probe target on the hub; control() only records it.
class NibeRegisterNumber : public number::Number, public Component {
 public:
  void setup() override;
  void control(float value) override;
  void set_hub(NibeBus *hub) { this->hub_ = hub; }

 protected:
  NibeBus *hub_{nullptr};
};
#endif

#ifdef USE_SWITCH
/// Arm/off gate for the emulator: booting passive, on = start the role, off = kill switch.
class NibeSwitch : public switch_::Switch, public Component {
 public:
  void set_hub(NibeBus *hub) { this->hub_ = hub; }

 protected:
  void write_state(bool state) override;  // defined out-of-line (NibeBus is incomplete here)

  NibeBus *hub_{nullptr};
};
#endif  // USE_SWITCH

/// Button entity that pushes a key frame (or re-announces) through the hub.
#ifdef USE_BUTTON

class NibeButton : public button::Button, public Component {
 public:
  void set_hub(NibeBus *hub) { this->hub_ = hub; }
  void set_key(uint8_t key) {
    this->key_ = key;
    this->announce_ = false;
  }
  void set_announce() { this->announce_ = true; }
  void set_read_register() { this->read_register_ = true; }

 protected:
  void press_action() override;

  uint8_t key_{0};
  bool announce_{false};
  bool read_register_{false};
  NibeBus *hub_{nullptr};
};
#endif

/// Fixed allocations, all in DRAM (ISR-visible).
static constexpr size_t TX_QUEUE_N = 8;      // pending frames deliverable on our polls
static constexpr size_t TX_FRAME_MAX = 64;  // engine frame bytes incl. ETX
static constexpr size_t WIRE_RING_N = 512;   // captured bytes between loop passes
static constexpr size_t TX_NOTICE_N = 8;     // ISR->task tx-log notifications

/// Nibe legacy-bus engine: the transport hooks run in the ISR and only capture bytes, track wire
/// mechanics (poll pair, grant, close, frame boundaries) and emit scheduled replies (ADR-0001).
/// The frame parser and every decode run in loop(), over the wire ring.
// ---------------------------------------------------------------------------
// Wire-measured engine timings — the facts live in docs/protocol.md:
//   POLL_REPLY_DELAY_US   masters never hear byte-instant replies; 2400 us earns grants
//   GRANT_FRAME_DELAY_US  digestible frames start ~+160 us after the grant
//   TX_BYTE_GAP_US        native nodes pace bytes (110-285 us idle, ~650 us/byte)
//   FRAME_CLOSE_DELAY_US  sender-closes: our ETX ~+670 us after the master receipt
//   PUSH_ACK_DELAY_US     pushes are receipt-ACKed at ~15.4 ms; earlier unproven
//   ANNOUNCE_BURST/BREATH 2 ENQ polls then 2 bare ACKs — emulator policy,
//                          not a wire fact (hypotheses.md 1; constants in nibe_queries.h)
//   NO_POLL_TIMEOUT_S     safety: drop the outbound queue when our polls stop
static constexpr uint32_t POLL_REPLY_DELAY_US = 2400;
static constexpr uint32_t GRANT_FRAME_DELAY_US = 160;
static constexpr uint32_t TX_BYTE_GAP_US = 180;
static constexpr uint32_t FRAME_CLOSE_DELAY_US = 670;
static constexpr uint32_t PUSH_ACK_DELAY_US = 15400;
static constexpr uint32_t NO_POLL_TIMEOUT_S = 30;

class NibeBus : public Component, public ExchangeSink, public QueryContext {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_emulator_role(bool on) { this->emulator_intent_ = on; }
  /// runtime arm/off gate (switch entity); the ISR paths check emulating()
  void set_armed(bool armed) { this->armed_ = armed; }
  /// emulator intent AND a transmit-capable transport AND the arm switch — never one alone
  /// (called from the ISR: keep it IRAM-safe and inline)
  bool IRAM_ATTR emulating() const {
    return this->emulator_intent_ && this->armed_ && this->transmit_capable_();
  }
  /// cmd-0x40 data[1] in request AND key frames: a relaid system value, no protocol meaning (commands.md).
  void set_request_byte(uint8_t b) { this->request_byte_ = b; }
  void set_address(uint16_t address) {
    this->my_addr_ = address;
    this->parser_.set_address(address);
  }
  void set_bus(rs485_bus::RS485Bus *bus) { this->bus_ = bus; }
  /// true once the master has delivered all three text rows (ISR-safe read)
  bool IRAM_ATTR rows_complete_() const { return (this->rows_seen_ & 0x07) == 0x07; }
  void set_menu_reply(bool enable) { this->menu_reply_ = enable; }
#ifdef USE_TIME
  /// D4 clock reply source: answering a 0xD4 sets the SYSTEM CLOCK — opt-in, silenced unless synced/valid.
  void set_time_source(time::RealTimeClock *t) { this->time_source_ = t; }
#endif
  void set_d4_reply(bool enable) { this->d4_reply_ = enable; }
  /// tail byte of the D4 clock answer (0 = auto: day-of-week; see hypotheses.md)
  void set_d4_tail(uint8_t tail) { this->d4_tail_ = tail; }
#ifdef USE_NIBE_DISPLAY_WEB
  /// Virtual-display web page: port 0 disables; served from loop() so rendering never touches wire timing.
  void set_display_web_port(uint16_t port) { this->web_port_ = port; }
  void serve_display_web();
#endif
  /// Knob placement (0-255, 127 = vertical): sends [KEY_KNOB, pos, 00, 00] on cmd-40 data[1] (commands.md).
  void set_knob_placement(uint8_t position);
  /// 0 = summary only, 1 = + one line per frame/exchange (default), 2 = + raw bytes
  void set_log_level(int level) { this->log_level_ = level; }

  /// Python glue: attach this engine to an rs485_bus (must run before bus setup).
  void attach_to(rs485_bus::RS485Bus *bus) {
    this->bus_ = bus;
    bus->attach_engine(this, rs485_bus::EngineHooks{&NibeBus::rx_byte_trampoline_, &NibeBus::gap_trampoline_,
                                                    &NibeBus::tx_done_trampoline_});
  }

  // ---- task-side entity plumbing ------------------------------------------

#ifdef USE_TEXT_SENSOR
  void attach_row_sensor(uint8_t row, text_sensor::TextSensor *s) { this->row_sinks_.push_back({row, s}); }
  void attach_key_name(text_sensor::TextSensor *s) { this->key_name_sink_ = s; }
  /// Raw 16-bit sensor-block word as hex ("0x3400") — for non-thermal words that /10-°C rendering cannot carry.
  void attach_f2025_word_hex(uint8_t idx, text_sensor::TextSensor *s) {
    this->f2025_word_hex_sinks_.push_back({idx, s});
  }
  /// Raw data bytes (hex) of the F2025's answer to cmd N — delivered to the decoding watch.
  void attach_f2025_frame_hex(uint8_t cmd, text_sensor::TextSensor *s) {
    this->f2025_frame_hex_sinks_.push_back({cmd, s});
  }
#endif
#ifdef USE_SENSOR
  void attach_counter(uint8_t which, sensor::Sensor *s) { this->counter_sinks_.push_back({which, s}); }
  /// Generic F2025 register: u16/s16 LE at byte `offset` of cmd's answer, times `scale` (docs/commands.md).
  struct F2025RegSpec {
    uint8_t cmd;
    uint8_t offset;
    float scale;
    bool is_signed;
  };
  void attach_f2025_register(uint8_t cmd, uint8_t offset, float scale, bool is_signed,
                             sensor::Sensor *s) {
    this->f2025_reg_sinks_.push_back({F2025RegSpec{cmd, offset, scale, is_signed}, s});
  }
  void attach_gap_sensor(sensor::Sensor *s) { this->gap_sink_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void attach_polled(binary_sensor::BinarySensor *s) { this->polled_sink_ = s; }
#endif

  /// Queue a key press frame for delivery on our next poll (emulator only).
  bool queue_key(uint8_t key);
  /// Same, with an explicit cmd-40 data[1] — used for knob events [KEY_KNOB, placement, 0, 0].
  bool queue_key_number(uint8_t key, uint8_t value);
  /// Pin the probe register for the read button (number entity, task side).
  void set_register_address(uint8_t r) { this->register_addr_ = r; }
  /// Queue a register read — cmd 0x72, data `05 <reg>`, delivered on our next granted poll (emulator only).
  bool queue_register_read();
  /// Refill the repeatable queue while the request phase is live.
  void refill_queries_();
  /// Re-announce: clears the rows latch and re-arms the request pump (arm switch or Re-announce button).
  bool trigger_announce();

  uint16_t get_address() const { return this->my_addr_; }
  /// what this build is: a sniffer, or an emulator that is transmitting, armed, or unable to
  const char *role_name() const {
    if (!this->emulator_intent_)
      return "sniffer";
    if (!this->transmit_capable_())
      return "emulator (transport passive)";
    return this->armed_ ? "emulator" : "emulator (unarmed)";
  }

 protected:
  /// the transport may drive the line (config `active: true`, resolved in the transport's setup)
  bool IRAM_ATTR transmit_capable_() const { return this->bus_ != nullptr && this->bus_->bus_active(); }

  // ---- queries (task side): the engine as a QueryContext ----------------------
  bool send(uint8_t cmd, const uint8_t *data, size_t len, Repeat repeat) override;
  bool content_complete() override { return this->rows_seen_ != 0; }
  bool queue_empty() override;
  bool queue_has_once() override;
  uint32_t queue_used() override;
  uint8_t request_byte() const override { return this->request_byte_; }
  /// queue tail after skipping expired repeatables (the peek the poll policy uses)
  uint32_t live_tail_();

  // ---- engine (ISR-safe) side ----------------------------------------------
  static void IRAM_ATTR rx_byte_trampoline_(void *ctx, uint8_t byte, uint32_t ts, uint8_t flags) {
    static_cast<NibeBus *>(ctx)->rx_byte_(byte, ts, flags);
  }
  static void IRAM_ATTR gap_trampoline_(void *ctx, uint32_t ts) { static_cast<NibeBus *>(ctx)->gap_(ts); }
  static void IRAM_ATTR tx_done_trampoline_(void *ctx, uint32_t ts) { static_cast<NibeBus *>(ctx)->tx_done_(ts); }

  void rx_byte_(uint8_t byte, uint32_t ts, uint8_t flags);
  void gap_(uint32_t ts);
  void tx_done_(uint32_t ts) {}

  bool IRAM_ATTR reply_to_poll_(uint32_t ts, uint8_t reply);  // one control byte as our poll reply
  bool IRAM_ATTR deliver_frame_(uint32_t ts);                  // frame + ETX from the queue head
  /// Wire mechanics only (ISR): poll pair, grant, close, frame boundaries — never checksums or meaning.
  void IRAM_ATTR track_byte_(uint8_t b, uint32_t ts, bool marked);
  void IRAM_ATTR push_wire_(uint8_t b, uint32_t ts, uint8_t flags);
  void IRAM_ATTR push_wire_gap_(uint32_t ts);
  void IRAM_ATTR push_tx_notice_(uint32_t ts, uint8_t kind, uint8_t byte, uint8_t len);
  /// Task side: drain the wire ring through the parser, and the tx notices into the log.
  void parse_wire_();
  void drain_tx_notices_();
  void tail_(uint32_t ts, char *out, size_t cap);
  void on_poll(uint32_t ts, uint16_t addr) override;
  void on_ctrl(uint32_t ts, uint8_t byte, uint16_t ctx) override;
  void on_frame(uint32_t ts, uint8_t cmd, uint16_t sender, const uint8_t *data, uint8_t len,
                bool in_our_slot) override;
  void on_frame_bad(uint32_t ts, uint8_t cmd, uint16_t sender, uint8_t len) override;
  void on_anomaly(uint32_t ts, uint8_t code) override;
  void on_gap(uint32_t ts, uint32_t gap_us) override;
  void on_len_rescue(uint32_t ts, uint8_t declared_len) override;

#ifdef USE_ESP32
  /// 125 us esp_timer tick (IRAM callback, esp_timer task): paced frame feeding + delayed replies (protocol.md).
  static void IRAM_ATTR poll_reply_tick_(void *arg);
  esp_timer_handle_t reply_timer_{nullptr};
#endif
  /// fires aged push receipts when the line is quiet (loop context)
  void emulator_push_ack_tick_();


  /// ISR tracker: just enough structure to answer polls and arm grants. The full parse is task-side.
  enum WireState : uint8_t {
    WS_BOUNDARY,   // exchange boundary: poll-pair candidate, frame start, or stray control
    WS_POLL_HI,    // seen the hi byte of a poll pair
    WS_CTRL,       // response slot of the just-polled node
    WS_FRAME_HDR,  // skipping sender hi/lo and len
    WS_FRAME_LEN,  // reading the declared data length
    WS_FRAME_BODY, // counting data bytes + checksum
    WS_POST_FRAME, // frame delivered: expect ETX (an ACK may interleave)
    WS_HUNT,       // desynced: only ETX or a gap re-opens the boundary
  };
  WireState ws_{WS_BOUNDARY};
  uint8_t ws_poll_hi_{0};
  uint8_t ws_skip_{0};
  uint16_t ws_cnt_{0};

  bool emulator_intent_{false};  // config intent: this build impersonates the panel
  volatile bool armed_{false};  // staged: only the arm switch (or switchless
                                  // host builds, armed in setup()) can raise this
  uint16_t my_addr_{ADDR_DISPLAY};
  rs485_bus::RS485Bus *bus_{nullptr};
  bool menu_reply_{true};
  int log_level_{1};

  // Push receipts: a loop-only ring — the parser arms, emulator_push_ack_tick_ fires.
  volatile uint32_t push_acks_[4];  // arm timestamps, µs, indexed wr/rd % 4
  volatile uint8_t push_ack_wr_{0};
  volatile uint8_t push_ack_rd_{0};
  uint8_t request_byte_{0x88};          // relaid system value in cmd-40 frames
  uint8_t knob_pos_{127};               // panel knob placement (mid/vertical)
  uint8_t register_addr_{0};            // probe target pinned by the register-address number

  // Delayed replies (mute-turnaround): armed in the RX ISR; fired by the tick (device) or loop (host).
  static constexpr uint8_t RP_NONE = 0, RP_CTRL = 1, RP_FRAME = 2;
  volatile uint32_t reply_fire_at_us_{0};  // 0 = nothing pending (µs since boot)
  volatile uint8_t reply_kind_{RP_NONE};
  volatile uint8_t reply_ctrl_var_{0};  // RP_CTRL: the byte to transmit
  volatile uint32_t reply_delay_var_{2400};  // the window this reply was armed with
  bool we_own_close_{false};                   // our delivered frame is awaiting its receipt&close
  volatile uint32_t reply_drops_{0};  // line spoke inside the turnaround window

  void IRAM_ATTR arm_reply_(uint8_t kind, uint8_t ctrl_var, uint32_t ts, uint32_t delay_us);
  void IRAM_ATTR fire_pending_reply_(uint32_t now_us);
  void IRAM_ATTR feed_paced_(uint32_t now_us);  // one byte per cadence step while a frame is in flight
  // Pops expired announce-requests at fire entry: txq_tail_ stays single-writer (tick/loop, never the UART ISR).
  void IRAM_ATTR purge_stale_requests_();

  // Paced frame delivery: one feeding context per build (tick on device, loop on host) — never both.
  volatile uint8_t paced_buf_[TX_FRAME_MAX];
  volatile uint8_t paced_n_{0};
  volatile uint8_t paced_k_{0};
  volatile uint32_t paced_start_us_{0};
  volatile uint32_t paced_prev_feed_us_{0};
  volatile uint32_t paced_gap_min_{0};
  volatile uint32_t paced_gap_max_{0};
  // task-side mirror of the last armed frame for the same-format tx log
  volatile uint8_t tx_log_buf_[TX_FRAME_MAX];
  volatile uint8_t tx_log_len_{0};

  // own reply bookkeeping (echo discrimination)
  uint8_t reply_byte_{0};
  volatile bool heard_since_tx_{true};  // ISR: the first byte after our reply may be its echo
  bool slot_ours_{false};   // true while inside an exchange for our address
  bool enq_pending_{false};  // we ENQed; the next master ACK (06) is our grant, even
                             // across a gap — but any poll pair in between revokes it
  volatile uint32_t reply_ts_{0};
  uint32_t byte_us_{521};  // 10 bit-times at 19200

  // frames queued for delivery on our polls (task produces, ISR consumes)
  struct PendingTx {
    uint8_t len;
    bool repeatable;  // a request the engine may expire once content arrives
    uint8_t buf[TX_FRAME_MAX - 1];
  };
  PendingTx tx_queue_[TX_QUEUE_N];
  volatile uint32_t txq_head_{0}, txq_tail_{0};

  // wire ring: captured bytes (ISR produces, loop parses) — replaces the event ring + arena
  WireRecord wire_[WIRE_RING_N];
  volatile uint32_t wire_head_{0}, wire_tail_{0};
  volatile uint32_t wire_overflow_{0};
  volatile bool wire_lost_{false};  // ISR dropped bytes: the parser must resync

  // tx notices: ISR emits, loop logs (the wire itself is already recorded by the transport)
  enum TxNoticeKind : uint8_t { TXN_REPLY = 1, TXN_FRAME, TXN_FEED_DONE, TXN_TX_REFUSED };
  struct TxNotice {
    uint32_t ts;
    uint8_t kind;
    uint8_t byte;
    uint8_t len;
  };
  TxNotice tx_notices_[TX_NOTICE_N];
  volatile uint32_t tx_notice_head_{0}, tx_notice_tail_{0};

  ExchangeParser parser_;

  // outbound commands as units (ADR-0003); the scheduler owns repeat policy and the poll reply
  ContentRequestQuery content_query_;
  KeyQuery key_query_;
  RegisterQuery register_query_;
  QueryScheduler scheduler_;

  // counters
  volatile uint32_t last_own_poll_ms_{0};
  volatile uint32_t polls_own_{0}, polls_other_{0};
  volatile uint32_t echo_filtered_{0}, anomalies_{0}, tx_sent_{0}, tx_refused_{0};
  volatile uint32_t last_gap_us_{0};

  // Content-request pump (protocol.md "The display slot"): refills while the master is unready.
  volatile uint8_t rows_seen_{0};  // bit0=row2, bit1=row3, bit2=row4 (from master)

  // ---- task side state -------------------------------------------------------
  uint32_t last_ev_ts_{0};  // µs stamp of the handled event before this one (log tail)
  uint32_t next_summary_ms_{0};

#ifdef USE_TEXT_SENSOR
  struct RowSink {
    uint8_t row;
    text_sensor::TextSensor *sensor;
  };
  std::vector<RowSink> row_sinks_;
  text_sensor::TextSensor *key_name_sink_{nullptr};
  std::vector<std::pair<uint8_t, text_sensor::TextSensor *>> f2025_word_hex_sinks_{};
  std::vector<std::pair<uint8_t, text_sensor::TextSensor *>> f2025_frame_hex_sinks_{};
#endif
#ifdef USE_NIBE_DISPLAY_WEB
  // ---- display-page state: the frame handler writes, the page builders read — same task context ----
  uint8_t sym_raw_[3]{};
  uint8_t sym_raw_len_{0};
  char snap_rows_[3][48]{};  // rows 2/3/4 text, latin-1 -> utf-8 handled
  void web_handle_action_(const std::string &action);
  std::string build_display_page_();
  std::string build_display_json_();
  uint16_t web_port_{0};
  std::unique_ptr<socket::ListenSocket> web_listen_{};
  std::unique_ptr<socket::Socket> web_client_{};
  std::string web_request_{};
  std::string web_response_{};
  size_t web_sent_{0};
  bool web_req_complete_{false};
  uint32_t web_client_ms_{0};
  uint32_t web_listen_try_ms_{0};
#endif
#ifdef USE_TIME
  time::RealTimeClock *time_source_{nullptr};
#endif
  // D4 clock answer config: opt-in and gated on a valid synced clock.
  bool d4_reply_{false};
  uint8_t d4_tail_{0x01};
#ifdef USE_SENSOR
  std::vector<std::pair<uint8_t, sensor::Sensor *>> counter_sinks_;
  std::vector<std::pair<F2025RegSpec, sensor::Sensor *>> f2025_reg_sinks_{};
  sensor::Sensor *gap_sink_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *polled_sink_{nullptr};
#endif
};

}  // namespace esphome::nibe
