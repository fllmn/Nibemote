#include "nibe_codec.h"

namespace esphome::nibe {

namespace {
uint16_t le16_(const uint8_t *p) { return (uint16_t) p[0] | ((uint16_t) p[1] << 8); }
uint8_t bcd_encode_(uint8_t v) { return (uint8_t) (((v / 10) << 4) | (v % 10)); }
}  // namespace

bool f2025_word(const FrameView &f, uint8_t index, int16_t *raw) {
  size_t off = (size_t) index * 2;
  if (raw == nullptr || f.data == nullptr || off + 2 > f.len)
    return false;
  *raw = (int16_t) le16_(&f.data[off]);
  return true;
}

float celsius_from_tenths(int16_t raw) { return (float) raw / 10.0f; }

bool f2025_register(const FrameView &f, uint8_t offset, bool is_signed, float scale, float *value) {
  if (value == nullptr || f.data == nullptr || (size_t) offset + 2 > f.len)
    return false;
  uint16_t raw = le16_(&f.data[offset]);
  *value = (is_signed ? (float) (int16_t) raw : (float) raw) * scale;
  return true;
}

bool parse_register_request(const FrameView &f, uint8_t *reg) {
  if (reg == nullptr || f.data == nullptr || f.cmd != CMD_REG_READ || f.len != 2 || f.data[0] != REG_PREFIX)
    return false;
  *reg = f.data[1];
  return true;
}

bool is_register_answer(const FrameView &f, uint16_t expected_addr) {
  return f.cmd == CMD_REG_READ && f.sender == expected_addr;
}

uint8_t d4_tail_byte(uint8_t configured, uint8_t day_of_week) {
  return configured != 0 ? configured : day_of_week;
}

size_t parse_hex_bytes(const char *s, uint8_t *out, size_t cap) {
  if (s == nullptr || out == nullptr)
    return 0;
  size_t n = 0;
  int hi = -1;
  for (; *s != 0; s++) {
    const char c = *s;
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else if (c == ' ')
      continue;
    else
      return 0;  // not hex
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= cap)
        return 0;  // would overflow the caller's buffer
      out[n++] = (uint8_t) ((hi << 4) | v);
      hi = -1;
    }
  }
  return hi < 0 ? n : 0;  // an odd digit count is not a byte string
}

bool append_checksum(uint8_t *frame, size_t len, size_t cap, size_t *out_len) {
  if (frame == nullptr || out_len == nullptr || len == 0 || len + 1 > cap)
    return false;
  frame[len] = checksum8(frame, len);
  *out_len = len + 1;
  return true;
}

void encode_d4_clock(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                     uint8_t second, uint8_t tail, uint8_t out[7]) {
  out[0] = bcd_encode_((uint8_t) (year % 100));
  out[1] = bcd_encode_(month);
  out[2] = bcd_encode_(day);
  out[3] = bcd_encode_(hour);
  out[4] = bcd_encode_(minute);
  out[5] = bcd_encode_(second);
  out[6] = tail;
}

}  // namespace esphome::nibe
