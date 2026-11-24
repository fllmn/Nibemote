#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nibe_frame.h"

namespace esphome::nibe {

/// One captured wire fact: a byte with its ninth bit, or a silence marker.
struct WireRecord {
  uint32_t ts;
  uint8_t byte;
  uint8_t flags;
};
static constexpr uint8_t WRF_GAP = 0x01;     // the bus fell silent; byte is unused
static constexpr uint8_t WRF_MARKED = 0x02;  // the byte's ninth (marking) bit read 1

enum Anomaly : uint8_t {
  AN_DESYNC_HUNT,       // checksum failed; hunting for the next ETX/gap boundary
  AN_FRAME_ABORT,       // declared frame len exceeds capacity; syncing
  AN_OWN_POLL_TIMEOUT,  // emulator: master stopped polling us (delivery queue dropped)
  AN_STRAY_CTRL,        // control byte seen at a place it shouldn't be (info)
};

/// Task-context consumer of decoded exchanges. Virtual is fine here: this never runs in the ISR (ADR-0001).
class ExchangeSink {
 public:
  virtual ~ExchangeSink() = default;
  virtual void on_poll(uint32_t ts, uint16_t addr) = 0;
  /// marked: the byte's ninth bit read 1 (polls, ENQ and the master's close are marked)
  virtual void on_ctrl(uint32_t ts, uint8_t byte, uint16_t ctx, bool marked) = 0;
  /// Only called for frames whose checksum verified (after the len-lie rescue).
  virtual void on_frame(uint32_t ts, uint8_t cmd, uint16_t sender, const uint8_t *data, uint8_t len,
                        bool in_our_slot) = 0;
  virtual void on_frame_bad(uint32_t ts, uint8_t cmd, uint16_t sender, uint8_t len) = 0;
  virtual void on_anomaly(uint32_t ts, uint8_t code) = 0;
  virtual void on_gap(uint32_t ts, uint32_t gap_us) = 0;
  /// A frame declared one byte less than it carried; the parser absorbed the extra byte and retested.
  virtual void on_len_rescue(uint32_t ts, uint8_t declared_len) = 0;
};

/// Frame parser over captured bytes: boundaries, checksum, len-lie rescue, payload assembly.
/// Task context only (ADR-0001): plain C++, no transport, no ESPHome types.
class ExchangeParser {
 public:
  void set_sink(ExchangeSink *sink) { this->sink_ = sink; }
  void set_address(uint16_t address) { this->my_addr_ = address; }

  void feed(const WireRecord &rec);
  /// Drop partial frame state (used when the wire ring lost bytes and the stream is torn).
  void reset();

  /// The node polled in the open exchange: the context a frame in that slot belongs to.
  uint16_t polled_addr() const { return this->polled_addr_; }
  uint32_t frames_ok() const { return this->frames_ok_; }
  uint32_t frames_bad() const { return this->frames_bad_; }
  /// worst byte-to-byte gap inside the last completed frame (µs)
  uint32_t last_frame_gap_us() const { return this->f_gap_report_us_; }

 private:
  enum State : uint8_t {
    P_BOUNDARY,  // exchange boundary: poll-pair candidate, frame start, or stray control
    P_POLL_HI,   // seen the hi byte of a poll pair
    P_CTRL,      // response slot of the just-polled node
    P_F_SENDER_HI,
    P_F_SENDER_LO,
    P_F_LEN,
    P_F_DATA,
    P_F_CHK,
    P_POST_FRAME,  // frame delivered: expect ETX (an ACK may interleave)
    P_HUNT,        // desynced: only ETX re-opens the boundary (a gap also does)
  };

  void byte_(uint8_t b, uint32_t ts);
  void boundary_(uint8_t b, uint32_t ts);
  void poll_lo_(uint8_t b, uint32_t ts);
  void ctrl_(uint8_t b, uint32_t ts);
  void begin_frame_(uint8_t cmd);
  void chk_(uint8_t b, uint32_t ts);
  void post_frame_(uint8_t b, uint32_t ts);
  void gap_(uint32_t ts);

  ExchangeSink *sink_{nullptr};
  uint16_t my_addr_{ADDR_DISPLAY};
  State st_{P_BOUNDARY};
  bool slot_ours_{false};
  uint8_t poll_hi_{0};
  uint16_t polled_addr_{0};
  uint8_t cmd_{0};
  uint16_t sender_{0};
  uint8_t f_len_{0};
  uint8_t f_idx_{0};
  uint8_t xor_run_{0};
  bool chk_rescue_{true};  // one data-extension retry per frame (len-lie fix)
  uint32_t f_max_gap_us_{0};
  uint32_t f_gap_report_us_{0};
  uint32_t last_byte_ts_{0};
  bool marked_{false};  // ninth bit of the byte currently being parsed
  uint8_t f_data_[FRAME_MAX_DATA];
  uint32_t frames_ok_{0};
  uint32_t frames_bad_{0};
};

}  // namespace esphome::nibe
