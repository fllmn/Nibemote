#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/hal.h"  // IRAM_ATTR (the ISR shares these wire facts)
#include "nibe_protocol.h"

namespace esphome::nibe {

/// Boundary tokens shared by the ISR tracker and the task parser — one spelling, so the two cannot drift.
/// IRAM_ATTR: both callers on the ISR side need these in DRAM code (OTA writes disable the flash cache).
static inline IRAM_ATTR bool is_poll_hi(uint8_t b) { return b == 0x00 || b == 0x41; }
static inline IRAM_ATTR bool is_ctrl_byte(uint8_t b) {
  return b == CTRL_ACK || b == CTRL_ENQ || b == CTRL_NAK || b == CTRL_ETX;
}

}  // namespace esphome::nibe
