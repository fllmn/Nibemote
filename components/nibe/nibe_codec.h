#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nibe_protocol.h"

// Decode and encode helpers over a frame view — no ESPHome, so they unit-test with plain C++.

namespace esphome::nibe {

/// The parts of a frame a decoder needs: no timestamp, no transport, no entities.
struct FrameView {
  uint8_t cmd;
  uint16_t sender;
  const uint8_t *data;
  uint8_t len;
};

/// F2025 sensor block (cmd 0x65): word `index` as a signed LE 16-bit value.
bool f2025_word(const FrameView &f, uint8_t index, int16_t *raw);
/// F2025 sensor words are tenths of a degree.
float celsius_from_tenths(int16_t raw);
/// Generic answer field: u16/s16 LE at `offset`, scaled. False when the frame is too short.
bool f2025_register(const FrameView &f, uint8_t offset, bool is_signed, float scale, float *value);
/// 0x72 request payload `05 <reg>`; false when the frame is not a register request.
bool parse_register_request(const FrameView &f, uint8_t *reg);
/// A 0x72 frame from the node the request was addressed to: its answer (payload grammar still open).
bool is_register_answer(const FrameView &f, uint16_t expected_addr);

/// Passive 0x72 pairing: the answer belongs to the register the master last asked the polled node
/// about, and only until the master moves to another node (the node answers in its next turn).
class RegisterPairing {
 public:
  void request(uint8_t reg, uint16_t node) {
    this->reg_ = reg;
    this->node_ = node;
    this->valid_ = true;
  }
  void clear() { this->valid_ = false; }
  /// A poll opens an exchange; a poll to another node closes the pending pairing.
  void poll(uint16_t node) {
    if (node != this->node_)
      this->valid_ = false;
  }
  /// True (and yields the register) when this frame is that node's answer.
  bool answer(const FrameView &f, uint8_t *reg) const {
    if (!this->valid_ || !is_register_answer(f, this->node_))
      return false;
    if (reg != nullptr)
      *reg = this->reg_;
    return true;
  }

 private:
  uint8_t reg_{0};
  uint16_t node_{0};
  bool valid_{false};
};

/// D4 tail byte: the configured override, else the day of week (0 = auto).
uint8_t d4_tail_byte(uint8_t configured, uint8_t day_of_week);
/// Parse space-separated hex pairs ("72 05 11" or "720511") into bytes; 0 on odd/invalid/overflow.
size_t parse_hex_bytes(const char *s, uint8_t *out, size_t cap);
/// Append the XOR-8 checksum to a frame built without one; false when it would not fit.
bool append_checksum(uint8_t *frame, size_t len, size_t cap, size_t *out_len);
/// D4 answer payload: yy mm dd hh mm ss + tail (day-of-week, Sunday=1).
void encode_d4_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                     uint8_t second, uint8_t tail, uint8_t out[7]);

}  // namespace esphome::nibe
