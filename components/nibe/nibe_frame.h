#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/hal.h"  // IRAM_ATTR (the ISR shares these wire facts)

namespace esphome::nibe {

// --- Wire constants (facts: docs/protocol.md, docs/commands.md) ---

static constexpr uint8_t CTRL_ACK = 0x06;  // polled node: nothing to send / master grant
static constexpr uint8_t CTRL_ENQ = 0x05;  // polled node: I have a frame
static constexpr uint8_t CTRL_NAK = 0x15;
static constexpr uint8_t CTRL_ETX = 0x03;  // exchange close, sent by the frame's sender

static constexpr uint16_t ADDR_VVM300 = 0x000A;      // bus master
static constexpr uint16_t ADDR_DISPLAY = 0x00F9;      // panel we replace
static constexpr uint16_t ADDR_RELAY = 0x00F5;
static constexpr uint16_t ADDR_ACCESSORY = 0x00F1;
static constexpr uint16_t ADDR_F2025 = 0x41C9;

// panel -> master frames, cmd 0x40, data [b0, b1, 00, 00] (commands.md)
static constexpr uint8_t CMD_KEY = 0x40;
static constexpr uint8_t KEY_PLUS = 0x01;
static constexpr uint8_t KEY_MINUS = 0x02;
// knob event: id 0x03 (owner recall, unverified), data[1] = placement u8 (hypotheses.md)
static constexpr uint8_t KEY_KNOB = 0x03;
static constexpr uint8_t KEY_ENTER = 0x04;

// display announce/heartbeat frame: cmd 0x40, data 40 88 00 00 (commands.md)
static constexpr uint8_t CMD_ANNOUNCE = 0x40;

// master->display rows, sender 0x000A: 0x50 symbol strip, 0x51..0x53 rows 2..4 latin-1 (commands.md)
static constexpr uint8_t CMD_ROW_BASE = 0x50;

static constexpr uint8_t CMD_MENU_REQ = 0x22;  // master boot ping (empty frame)
static constexpr uint8_t CMD_MENU2 = 0x62;      // master menu request; reply data [0x0F]
static constexpr uint8_t CMD_KEEPALIVE = 0xFF;   // empty keepalive frame

// clock query (~64 s): the panel answers yy mm dd hh mm ss + day-of-week (protocol.md "The clock")
static constexpr uint8_t CMD_CLOCK = 0xD4;

// F2025 sensor block: empty push, 19-byte answer (9 LE s16 words + pad) — word map: commands.md
static constexpr uint8_t CMD_SENSOR = 0x65;

// F2025 status block: len-0 push, 16-byte answer, byte-packed (offset map: commands.md)
static constexpr uint8_t CMD_STATUS = 0x71;

// 0xC0: forum parameter map from a different product line — kept as a rejected citation (hypotheses.md)
static constexpr uint8_t CMD_PARAM = 0xC0;

// register scheme: read 0x72 `05 <reg>` (write 0x76 `05 <reg> <val>` — reads only, commands.md)
static constexpr uint8_t CMD_REG_READ = 0x72;
static constexpr uint8_t REG_PREFIX = 0x05;

/// Wire facts shared by the ISR tracker and the task parser — one spelling, so the two cannot drift.
/// IRAM_ATTR: both callers on the ISR side need these in DRAM code (OTA writes disable the flash cache).
static inline IRAM_ATTR bool is_poll_hi(uint8_t b) { return b == 0x00 || b == 0x41; }
static inline IRAM_ATTR bool is_ctrl_byte(uint8_t b) {
  return b == CTRL_ACK || b == CTRL_ENQ || b == CTRL_NAK || b == CTRL_ETX;
}

// XOR-8 over cmd, sender, len and data. Pure/ISR-safe.
static inline uint8_t checksum8(const uint8_t *data, size_t len) {
  uint8_t x = 0;
  for (size_t i = 0; i < len; i++)
    x ^= data[i];
  return x;
}

static constexpr size_t FRAME_MAX_DATA = 64;  // data cap we buffer for events/arena

/// Build a frame into out; returns total length incl. checksum — ETX is not appended (protocol.md: sender closes)
static inline size_t build_frame(uint8_t *out, size_t out_cap, uint8_t cmd, uint16_t sender,
                                 const uint8_t *data, size_t data_len) {
  size_t total = 4 + data_len + 1;
  if (out == nullptr || out_cap < total || data_len > 255)
    return 0;
  out[0] = cmd;
  out[1] = (uint8_t) (sender >> 8);
  out[2] = (uint8_t) (sender & 0xFF);
  out[3] = (uint8_t) data_len;
  for (size_t i = 0; i < data_len; i++)
    out[4 + i] = data[i];
  out[4 + data_len] = checksum8(out, 4 + data_len);
  return total;
}

}  // namespace esphome::nibe
