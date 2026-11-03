// Nibe legacy RS485 bus protocol (VVM300 + F2025) — wire constants.
//
// Reverse engineered in the Elektronikforumet thread t=13714 and confirmed
// live on a VVM300 + F2025 (see docs/PROTOCOL-SPEC.md). The VVM300 CPU
// (address 0x000A) is the bus master and polls nodes; we impersonate the
// display card (default address 0x00F9) so the heat pump keeps talking after
// the physical display is removed.
//
// Wire format (19200 baud 8N1, half duplex):
//   poll        : <addr:u16be>                          (2 bytes)
//   node answer : 06 ACK | 05 ENQ (has data) | 15 NAK   (within ~1-2 ms)
//   data frame  : <cmd> <sender:u16be> <len> <data...> <xor8 csum>
//   end exchange: 03 ETX
#pragma once

#include <cstdint>

namespace esphome {
namespace nibe {

static const uint8_t NIBE_ACK = 0x06;
static const uint8_t NIBE_NAK = 0x15;
static const uint8_t NIBE_ENQ = 0x05;
static const uint8_t NIBE_ETX = 0x03;
static const uint8_t NIBE_CMD_BUTTON = 0x40;   // button press / knob / boot announce
static const uint8_t NIBE_CMD_TEXT = 0x50;     // 0x50..0x53 = display rows 1..4 (live-verified)
static const uint8_t NIBE_CMD_PARAMS = 0xC0;   // parameter dump / write
static const uint8_t NIBE_CMD_BOOT_PING = 0x22;
static const uint8_t NIBE_CMD_MENU_REQ = 0x62;
static const uint16_t NIBE_ADDR_VVM_CPU = 0x000A;
static const uint16_t NIBE_ADDR_DISPLAY = 0x00F9;

// The button/knob/boot-announce frame data captured from the real display.
static const uint8_t NIBE_ANNOUNCE[4] = {0x40, 0x85, 0x00, 0x00};

// XOR of all bytes (plain — no 0x5C escape quirk in the legacy protocol).
inline uint8_t xor8_plain(const uint8_t *data, size_t len) {
  uint8_t chk = 0;
  for (size_t i = 0; i < len; i++)
    chk ^= data[i];
  return chk;
}

}  // namespace nibe
}  // namespace esphome
