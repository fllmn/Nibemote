#include "nibe_wire.h"

namespace esphome::nibe {

void ExchangeParser::feed(const WireRecord &rec) {
  if ((rec.flags & WRF_GAP) != 0) {
    this->gap_(rec.ts);
    return;
  }
  this->byte_(rec.byte, rec.ts);
}

void ExchangeParser::byte_(uint8_t b, uint32_t ts) {
  // intra-frame byte-gap tracking (diagnostics): body bytes only — the leading gap belongs to the exchange
  if (this->st_ >= P_F_SENDER_HI && this->st_ <= P_F_CHK) {
    uint32_t d = ts - this->last_byte_ts_;
    if (d > this->f_max_gap_us_)
      this->f_max_gap_us_ = d;
  }
  this->last_byte_ts_ = ts;

  switch (this->st_) {
    case P_BOUNDARY:
      this->boundary_(b, ts);
      break;
    case P_POLL_HI:
      this->poll_lo_(b, ts);
      break;
    case P_CTRL:
      this->ctrl_(b, ts);
      break;
    case P_F_SENDER_HI:
      this->sender_ = ((uint16_t) b) << 8;
      this->xor_run_ ^= b;
      this->st_ = P_F_SENDER_LO;
      break;
    case P_F_SENDER_LO:
      this->sender_ |= b;
      this->xor_run_ ^= b;
      this->st_ = P_F_LEN;
      break;
    case P_F_LEN:
      this->f_len_ = b;
      this->f_idx_ = 0;
      this->xor_run_ ^= b;
      if (b > FRAME_MAX_DATA) {  // declared len exceeds the buffer: resync rather than over-read
        this->sink_->on_anomaly(ts, AN_FRAME_ABORT);
        this->st_ = P_HUNT;
      } else if (b == 0) {
        this->st_ = P_F_CHK;
      } else {
        this->st_ = P_F_DATA;
      }
      break;
    case P_F_DATA:
      if (this->f_idx_ < FRAME_MAX_DATA)
        this->f_data_[this->f_idx_] = b;
      this->f_idx_++;
      this->xor_run_ ^= b;
      if (this->f_idx_ >= this->f_len_)
        this->st_ = P_F_CHK;
      break;
    case P_F_CHK:
      this->chk_(b, ts);
      break;
    case P_POST_FRAME:
      this->post_frame_(b, ts);
      break;
    case P_HUNT:
      if (b == CTRL_ETX) {
        this->sink_->on_ctrl(ts, b, 0);
        this->st_ = P_BOUNDARY;
      }
      break;
  }
}

/// Boundary byte dispatch: poll-pair hi candidates, stray control bytes, or a frame start.
void ExchangeParser::boundary_(uint8_t b, uint32_t ts) {
  if (b == 0x00 || b == 0x41) {  // poll hi: nodes are 00xx (VVM accessories/display), F2025 = 41xx
    this->poll_hi_ = b;
    this->st_ = P_POLL_HI;
  } else if (b == CTRL_ACK || b == CTRL_ENQ || b == CTRL_NAK || b == CTRL_ETX) {
    this->sink_->on_ctrl(ts, b, 0);
  } else {
    this->begin_frame_(b);
  }
}

/// Second byte of a poll pair: the polled slot opens.
void ExchangeParser::poll_lo_(uint8_t b, uint32_t ts) {
  uint16_t addr = ((uint16_t) this->poll_hi_ << 8) | b;
  this->polled_addr_ = addr;
  this->slot_ours_ = (addr == this->my_addr_);
  this->sink_->on_poll(ts, addr);
  this->st_ = P_CTRL;
}

/// Response/continuation slot of the just-polled exchange.
void ExchangeParser::ctrl_(uint8_t b, uint32_t ts) {
  if (b == CTRL_ACK || b == CTRL_ENQ || b == CTRL_NAK || b == CTRL_ETX) {
    this->sink_->on_ctrl(ts, b, this->polled_addr_);
    this->st_ = P_BOUNDARY;
  } else if (b == 0x00 || b == 0x41) {
    // node silent; master moved straight to the next poll
    this->poll_hi_ = b;
    this->st_ = P_POLL_HI;
  } else {
    // a frame arrives into the slot (rows after our ACK) or from a node after its ENQ/grant
    this->begin_frame_(b);
  }
}

void ExchangeParser::begin_frame_(uint8_t cmd) {
  this->cmd_ = cmd;
  this->xor_run_ = cmd;
  this->f_max_gap_us_ = 0;  // fresh intra-frame spacing measurement
  this->st_ = P_F_SENDER_HI;
}

void ExchangeParser::chk_(uint8_t b, uint32_t ts) {
  if (b != this->xor_run_ && this->chk_rescue_) {
    // len-lie rescue: some frames carry one byte more than len declares (0x71) — absorb, retest chk, once per frame
    this->chk_rescue_ = false;
    if (this->f_idx_ < FRAME_MAX_DATA) {
      this->f_data_[this->f_idx_++] = b;
      this->f_len_++;
      this->xor_run_ ^= b;
    }
    this->sink_->on_ctrl(ts, 0x00, 0);  // marker: len was off-by-one
    return;                             // stay in P_F_CHK — the next byte is the new chk candidate
  }
  this->chk_rescue_ = true;
  if (b != this->xor_run_) {
    this->frames_bad_++;
    this->sink_->on_frame_bad(ts, this->cmd_, this->sender_, this->f_len_);
    this->sink_->on_anomaly(ts, AN_DESYNC_HUNT);
    this->st_ = P_HUNT;
    return;
  }
  this->frames_ok_++;
  this->f_gap_report_us_ = this->f_max_gap_us_;
  this->sink_->on_frame(ts, this->cmd_, this->sender_, this->f_data_, this->f_idx_, this->slot_ours_);
  this->st_ = P_POST_FRAME;
}

/// Frame is on the wire; expect the sender's ETX (an ACK may interleave).
void ExchangeParser::post_frame_(uint8_t b, uint32_t ts) {
  if (b == CTRL_ETX || b == CTRL_ENQ) {
    // ETX closes the exchange; an ENQ from the polled node answers a master push and re-arms the parse
    this->sink_->on_ctrl(ts, b, 0);
    this->st_ = P_BOUNDARY;
  } else if (b == CTRL_ACK || b == CTRL_NAK) {
    this->sink_->on_ctrl(ts, b, 0);
  } else if (b == 0x00 || b == 0x41) {
    // missing ETX; salvage by treating the pair as the next poll and move on
    this->sink_->on_anomaly(ts, AN_STRAY_CTRL);
    this->poll_hi_ = b;
    this->st_ = P_POLL_HI;
  } else {
    this->sink_->on_anomaly(ts, AN_DESYNC_HUNT);
    this->st_ = P_HUNT;
  }
}

void ExchangeParser::gap_(uint32_t ts) {
  this->sink_->on_gap(ts, ts - this->last_byte_ts_);
  this->st_ = P_BOUNDARY;
}

}  // namespace esphome::nibe
