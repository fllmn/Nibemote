#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/hal.h"

namespace esphome::nibe {

/// What the panel is showing: the three text rows, the row-1 symbol strip, and the knob.
/// The `seen` bits are volatile because the ISR's request purge reads them; the text is task-only.
class ScreenState {
 public:
  static constexpr uint8_t ROWS = 3;        // rows 2..4
  static constexpr size_t ROW_CAP = 48;     // utf-8 bytes per row, NUL included
  static constexpr size_t SYM_CAP = 3;      // row-1 symbol strip bytes

  /// Record a master-pushed text row (2..4) and mark it seen.
  void set_row(uint8_t row, const char *utf8);
  /// Record the row-1 symbol strip.
  void set_symbols(const uint8_t *raw, uint8_t len);

  /// The first content row has arrived: the request phase is over.
  bool IRAM_ATTR any() const { return this->seen_ != 0; }
  /// All three text rows have arrived.
  bool IRAM_ATTR complete() const { return (this->seen_ & 0x07) == 0x07; }
  uint8_t seen() const { return this->seen_; }
  void reset_rows() { this->seen_ = 0; }

  /// "" until that row has been seen.
  const char *row(uint8_t row) const;
  uint8_t symbols_len() const { return this->sym_len_; }
  uint8_t symbol(uint8_t i) const { return i < this->sym_len_ ? this->sym_[i] : 0; }

  void set_knob(uint8_t placement) { this->knob_ = placement; }
  uint8_t knob() const { return this->knob_; }

 private:
  volatile uint8_t seen_{0};  // bit0=row2, bit1=row3, bit2=row4
  char rows_[ROWS][ROW_CAP]{};
  uint8_t sym_[SYM_CAP]{};
  uint8_t sym_len_{0};
  uint8_t knob_{127};  // mid placement (vertical)
};

}  // namespace esphome::nibe
