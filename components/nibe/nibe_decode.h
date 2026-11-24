#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nibe_protocol.h"

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
/// BCD, e.g. 26 -> 0x26.
uint8_t bcd_encode(uint8_t v);
/// D4 answer payload: yy mm dd hh mm ss + tail (day-of-week, Sunday=1).
void encode_d4_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                     uint8_t second, uint8_t tail, uint8_t out[7]);

}  // namespace esphome::nibe
