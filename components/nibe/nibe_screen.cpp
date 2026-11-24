#include "nibe_screen.h"

namespace esphome::nibe {

void ScreenState::set_row(uint8_t row, const char *utf8) {
  if (!valid_row_(row))
    return;
  char *dst = this->rows_[row - 2];
  size_t i = 0;
  for (; utf8[i] != 0 && i + 1 < ROW_CAP; i++)
    dst[i] = utf8[i];
  dst[i] = 0;
  this->seen_ |= (uint8_t) (1u << (row - 2));
}

void ScreenState::set_symbols(const uint8_t *raw, uint8_t len) {
  this->sym_len_ = len < SYM_CAP ? len : SYM_CAP;
  for (uint8_t i = 0; i < this->sym_len_; i++)
    this->sym_[i] = raw[i];
}

const char *ScreenState::row(uint8_t row) const {
  if (!valid_row_(row))
    return "";
  return this->rows_[row - 2];
}

}  // namespace esphome::nibe
