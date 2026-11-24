// Plain-C++ tests for the pure decode/encode helpers (no ESPHome, no transport).
// Run with `make test`; the gate runs it after compiling the firmware.
#include <cstdio>

#include "nibe_codec.h"

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
    CHECK(f2025_word(f, 8, &w));               // last complete word: bytes 16..17
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
    const uint8_t long_req[3] = {0x05, 0x11, 0x00};
    FrameView f3{CMD_REG_READ, ADDR_VVM300, long_req, sizeof(long_req)};
    CHECK(!parse_register_request(f3, &reg));  // a read carries exactly `05 <reg>`
  }

  // --- register pairing: node-bound, one answer, closed by a poll to another node ---
  {
    const uint8_t ans[1] = {0x00};
    FrameView answer{CMD_REG_READ, ADDR_F2025, ans, sizeof(ans)};
    RegisterPairing p;
    uint8_t reg = 0;

    p.request(0x11, ADDR_F2025);
    p.poll(ADDR_F2025);  // the node's next turn is its answer's exchange
    CHECK(p.answer(answer, &reg) && reg == 0x11);
    p.clear();
    CHECK(!p.answer(answer, &reg));  // consumed after one answer

    p.request(0x22, ADDR_F2025);
    p.poll(ADDR_RELAY);  // master moved on
    CHECK(!p.answer(answer, &reg));

    p.request(0x33, ADDR_F2025);
    FrameView other{CMD_REG_READ, ADDR_RELAY, ans, sizeof(ans)};
    CHECK(!p.answer(other, &reg));  // another node is not this answer

    p.clear();
    p.request(0x44, ADDR_F2025);
    FrameView wrong{CMD_STATUS, ADDR_F2025, ans, sizeof(ans)};
    CHECK(!p.answer(wrong, &reg));
    CHECK(is_register_answer(answer, ADDR_F2025));
    CHECK(!is_register_answer(answer, ADDR_RELAY));
  }

  // --- hex parsing for the web raw-frame field ---
  {
    uint8_t out[8] = {0};
    CHECK(parse_hex_bytes("72 05 11", out, sizeof(out)) == 3);
    CHECK(out[0] == 0x72 && out[1] == 0x05 && out[2] == 0x11);
    CHECK(parse_hex_bytes("720511", out, sizeof(out)) == 3);
    CHECK(parse_hex_bytes("72 05 11 AB", out, sizeof(out)) == 4 && out[3] == 0xAB);
    CHECK(parse_hex_bytes("7", out, sizeof(out)) == 0);    // odd digit count
    CHECK(parse_hex_bytes("7G", out, sizeof(out)) == 0);   // not hex
    CHECK(parse_hex_bytes("", out, sizeof(out)) == 0);     // nothing
    CHECK(parse_hex_bytes("00 11 22 33", out, 3) == 0);    // would overflow the buffer

    uint8_t frame[8] = {0x72, 0x00, 0xF9, 0x02, 0x05, 0x11};
    size_t n = 0;
    CHECK(append_checksum(frame, 6, sizeof(frame), &n) && n == 7 && frame[6] == 0x9D);
    uint8_t query[5] = {0x32, 0x00, 0xF9, 0x00};
    CHECK(append_checksum(query, 4, sizeof(query), &n) && n == 5 && query[4] == 0xCB);
    CHECK(!append_checksum(frame, 8, sizeof(frame), &n));  // no room for the checksum
    CHECK(!append_checksum(frame, 0, sizeof(frame), &n));  // nothing to checksum

    uint8_t reg = 0;
    CHECK(panel_register_value(0x40, &reg) && reg == 0x06);
    CHECK(panel_register_value(0x44, &reg) && reg == 0x68);
    CHECK(panel_register_value(0xBF, &reg) && reg == 0x4B);
    CHECK(panel_register_value(0x41, &reg) && reg == 0x00);
    CHECK(!panel_register_value(0x3F, &reg));
    CHECK(!panel_register_value(0xC0, &reg));
  }

  // --- D4 clock answer: BCD yy mm dd hh mm ss + tail ---
  {
    uint8_t out[7] = {0};
    encode_d4_clock(2026, 11, 24, 9, 5, 7, 2, out);
    const uint8_t want[7] = {0x26, 0x11, 0x24, 0x09, 0x05, 0x07, 0x02};
    for (int i = 0; i < 7; i++)
      CHECK(out[i] == want[i]);

    uint8_t edge[7] = {0};
    encode_d4_clock(2000, 1, 2, 0, 0, 0, 1, edge);  // BCD zeros, single digits
    CHECK(edge[0] == 0x00 && edge[1] == 0x01 && edge[2] == 0x02 && edge[6] == 0x01);

    uint8_t n99[7] = {0};
    encode_d4_clock(2099, 99, 99, 23, 59, 59, 32, n99);  // upper BCD digits
    CHECK(n99[0] == 0x99 && n99[1] == 0x99 && n99[3] == 0x23 && n99[5] == 0x59);

    CHECK(d4_tail_byte(0, 3) == 3);  // 0 = auto: the day of week
    CHECK(d4_tail_byte(5, 3) == 5);  // configured override
  }

  if (failures == 0)
    std::printf("codec tests: all checks passed\n");
  else
    std::printf("codec tests: %d check(s) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
