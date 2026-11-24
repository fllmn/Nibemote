// Plain-C++ tests for the pure decode/encode helpers (no ESPHome, no transport).
// Run with `make test`; the gate runs it after compiling the firmware.
#include <cstdio>

#include "nibe_decode.h"

using namespace esphome::nibe;

static int failures = 0;

#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                         \
    }                                                     \
  } while (0)

int main() {
  // --- frame codec: build_frame lays out cmd, sender, len, data, XOR-8 ---
  {
    const uint8_t data[2] = {0x05, 0x11};
    uint8_t out[16];
    size_t n = build_frame(out, sizeof(out), CMD_REG_READ, ADDR_DISPLAY, data, sizeof(data));
    CHECK(n == 7);
    CHECK(out[0] == 0x72);
    CHECK(out[1] == 0x00 && out[2] == 0xF9);
    CHECK(out[3] == 0x02);
    CHECK(out[4] == 0x05 && out[5] == 0x11);
    CHECK(out[6] == checksum8(out, 6));
    CHECK(build_frame(out, 4, CMD_KEY, 0, data, sizeof(data)) == 0);  // too small
  }

  // --- F2025 sensor block: 19 bytes, signed LE words ---
  {
    const uint8_t block[19] = {0xBA, 0xFF, 0x0A, 0x00, 0x64, 0x00, 0x11, 0x00, 0x74, 0x01,
                              0x00, 0x40, 0x00, 0x00, 0x01, 0x80, 0xA0, 0x01, 0x20};
    FrameView f{CMD_SENSOR, ADDR_F2025, block, sizeof(block)};
    int16_t w = 0;
    CHECK(f2025_word(f, 0, &w) && w == -70);   // 0xFFBA
    CHECK(f2025_word(f, 1, &w) && w == 10);
    CHECK(f2025_word(f, 2, &w) && w == 100);
    CHECK(f2025_word(f, 4, &w) && w == 372);   // 0x0174
    CHECK(!f2025_word(f, 9, &w));              // past the 18 payload bytes
    CHECK(celsius_from_tenths(-70) == -7.0f);
    CHECK(celsius_from_tenths(372) == 37.2f);
  }

  // --- generic F2025 register: signed/unsigned, scale, bounds ---
  {
    const uint8_t block[4] = {0x9C, 0xFF, 0x2C, 0x01};
    FrameView f{CMD_STATUS, ADDR_F2025, block, sizeof(block)};
    float v = 0;
    CHECK(f2025_register(f, 0, true, 1.0f, &v) && v == -100.0f);
    CHECK(f2025_register(f, 0, false, 1.0f, &v) && v == 65436.0f);
    CHECK(f2025_register(f, 2, true, 0.5f, &v) && v == 150.0f);  // 300 * 0.5
    CHECK(!f2025_register(f, 3, true, 1.0f, &v));                // needs two bytes
  }

  // --- register request `05 <reg>` ---
  {
    const uint8_t req[2] = {0x05, 0x11};
    FrameView f{CMD_REG_READ, ADDR_VVM300, req, sizeof(req)};
    uint8_t reg = 0;
    CHECK(parse_register_request(f, &reg) && reg == 0x11);
    FrameView wrong_cmd{CMD_STATUS, ADDR_VVM300, req, sizeof(req)};
    CHECK(!parse_register_request(wrong_cmd, &reg));
    const uint8_t wrong_prefix[2] = {0x04, 0x11};
    FrameView f2{CMD_REG_READ, ADDR_VVM300, wrong_prefix, sizeof(wrong_prefix)};
    CHECK(!parse_register_request(f2, &reg));
    FrameView short_req{CMD_REG_READ, ADDR_VVM300, req, 1};
    CHECK(!parse_register_request(short_req, &reg));
  }

  // --- D4 clock answer: BCD yy mm dd hh mm ss + tail ---
  {
    CHECK(bcd_encode(0) == 0x00);
    CHECK(bcd_encode(9) == 0x09);
    CHECK(bcd_encode(26) == 0x26);
    CHECK(bcd_encode(99) == 0x99);
    uint8_t out[7] = {0};
    encode_d4_clock(2026, 11, 24, 9, 5, 7, 2, out);
    const uint8_t want[7] = {0x26, 0x11, 0x24, 0x09, 0x05, 0x07, 0x02};
    for (int i = 0; i < 7; i++)
      CHECK(out[i] == want[i]);
  }

  if (failures == 0)
    std::printf("decode tests: all checks passed\n");
  else
    std::printf("decode tests: %d check(s) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
