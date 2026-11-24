#include "nibe_decode.h"

namespace esphome::nibe {

bool f2025_word(const FrameView &f, uint8_t index, int16_t *raw) {
  size_t off = (size_t) index * 2;
  if (raw == nullptr || f.data == nullptr || off + 2 > f.len)
    return false;
  *raw = (int16_t) ((uint16_t) f.data[off] | ((uint16_t) f.data[off + 1] << 8));
  return true;
}

float celsius_from_tenths(int16_t raw) { return (float) raw / 10.0f; }

bool f2025_register(const FrameView &f, uint8_t offset, bool is_signed, float scale, float *value) {
  if (value == nullptr || f.data == nullptr || (size_t) offset + 2 > f.len)
    return false;
  uint16_t raw = (uint16_t) f.data[offset] | ((uint16_t) f.data[offset + 1] << 8);
  *value = (is_signed ? (float) (int16_t) raw : (float) raw) * scale;
  return true;
}

bool parse_register_request(const FrameView &f, uint8_t *reg) {
  if (reg == nullptr || f.data == nullptr || f.cmd != CMD_REG_READ || f.len != 2 || f.data[0] != REG_PREFIX)
    return false;
  *reg = f.data[1];
  return true;
}

uint8_t bcd_encode(uint8_t v) { return (uint8_t) (((v / 10) << 4) | (v % 10)); }

void encode_d4_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                     uint8_t second, uint8_t tail, uint8_t out[7]) {
  out[0] = bcd_encode((uint8_t) (year % 100));
  out[1] = bcd_encode(month);
  out[2] = bcd_encode(day);
  out[3] = bcd_encode(hour);
  out[4] = bcd_encode(minute);
  out[5] = bcd_encode(second);
  out[6] = tail;
}

}  // namespace esphome::nibe
