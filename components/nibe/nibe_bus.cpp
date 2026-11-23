#include "nibe_bus.h"
#include "esphome/core/time.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome::nibe {

static const char *const TAG = "nibe";

static const char *const KEY_NAMES[] = {"?", "PLUS", "MINUS", "?", "ENTER"};

static inline uint32_t IRAM_ATTR q_load_(volatile uint32_t *v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
static inline void IRAM_ATTR q_store_(volatile uint32_t *v, uint32_t val) { __atomic_store_n(v, val, __ATOMIC_RELEASE); }

static inline void hex_(char *out, size_t cap, const uint8_t *data, size_t len) {
  size_t p = 0;
  for (size_t i = 0; i < len && p + 3 < cap; i++) {
    p += (size_t) snprintf(out + p, cap - p, "%02X ", data[i]);
  }
}

// ------ engine side (runs in the bus ISR on hardware): byte FSM, replies, delivery ------

void IRAM_ATTR NibeBus::push_event_(const EventT &ev) {
  uint32_t h = q_load_(&this->ev_head_);
  uint32_t t = q_load_(&this->ev_tail_);
  uint32_t next = (h + 1) % EV_RING_N;
  if (next == t) {  // consumer too slow: drop newest, report in the summary
    this->ev_overflow_++;
    return;
  }
  this->events_[h] = ev;
  q_store_(&this->ev_head_, next);
}

bool IRAM_ATTR NibeBus::reply_to_poll_(uint32_t ts, uint8_t reply) {
  if (!this->emulating() || this->bus_ == nullptr)
    return false;
  if (!this->bus_->send_burst(&reply, 1, reply == CTRL_ENQ)) {
    this->tx_refused_++;
    this->push_event_(EventT{ts, EV_ANOMALY, AN_TX_REFUSED, 0, 0, 0});
    return false;
  }
  this->tx_sent_++;
  this->enq_pending_ = (reply == CTRL_ENQ);
  this->push_event_(EventT{ts, EV_TX, reply, 0, 0, 0});
  return true;
}

bool IRAM_ATTR NibeBus::deliver_frame_(uint32_t ts) {
  uint32_t h = q_load_(&this->txq_head_);
  uint32_t t = q_load_(&this->txq_tail_);
  if ((h - t + 2 * TX_QUEUE_N) % (2 * TX_QUEUE_N) == 0)
    return false;
  const PendingTx &tx = this->tx_queue_[t % TX_QUEUE_N];
  if (tx.len <= 1)  // only whole frames are queued (queue_frame_ builds cmd..chk); the guard cannot fire
    return false;
  uint8_t out[TX_FRAME_MAX];
  memcpy(out, tx.buf, tx.len);
  // sender-closes: master receipt 06 -> our closing 03 (FRAME_CLOSE_DELAY_US); no receipt = dying frame, never retried
  uint32_t next_tail = (t + 1) % (2 * TX_QUEUE_N);
  q_store_(&this->txq_tail_, next_tail);  // pop first: a refused send drops, never retries
  // native cadence: every node paces its bytes ~650 us (protocol.md) — contiguous bursts are never digested
  for (uint8_t i = 0; i < tx.len; i++) {
    this->paced_buf_[i] = out[i];
    this->tx_log_buf_[i] = out[i];  // task-side mirror for the same-format log
  }
  this->tx_log_len_ = tx.len;
  this->paced_n_ = tx.len;
  this->paced_k_ = 0;
  this->paced_start_us_ = ts;
  this->paced_prev_feed_us_ = 0;
  this->paced_gap_min_ = ~0u;
  this->paced_gap_max_ = 0;
  this->tx_sent_++;
  this->enq_pending_ = false;
  this->we_own_close_ = true;  // granted-frame sender closes after the master's receipt
  this->push_event_(EventT{ts, EV_TX, out[0], 0, (uint8_t) tx.len, 0});
  this->feed_paced_(ts);  // byte 0 is due right now
  return true;
}

/// Feed one byte per native cadence step; BUSY retries the SAME byte; a poll pair voids the feed (checked per byte)
void IRAM_ATTR NibeBus::feed_paced_(uint32_t now) {
  uint8_t k = this->paced_k_;
  uint8_t n = this->paced_n_;
  if (k >= n || n == 0)
    return;
  const uint32_t cadence = this->byte_us_ + TX_BYTE_GAP_US;
  while (k < n) {
    if (this->paced_n_ != n)
      return;  // voided mid-feed by a poll pair: leave the ISR's reset alone
    uint32_t due = this->paced_start_us_ + (uint32_t) k * cadence;
    if ((int32_t) (now - due) < 0)
      break;
    uint8_t b = this->paced_buf_[k];
    if (!this->bus_->send_burst(&b, 1, false)) {
      // BUSY = previous byte still draining (TX_DONE lag): this byte NOT sent — retry the SAME byte next tick
      if (this->paced_n_ != n)
        return;  // voided while we were at the UART: nothing retries
      this->tx_refused_++;
      this->paced_k_ = k;
      return;
    }
    if (this->paced_prev_feed_us_ != 0) {
      uint32_t g = now - this->paced_prev_feed_us_;
      if (g < this->paced_gap_min_)
        this->paced_gap_min_ = g;
      if (g > this->paced_gap_max_)
        this->paced_gap_max_ = g;
    }
    this->paced_prev_feed_us_ = now;
    k++;
    if (k == n)
      this->push_event_(EventT{now, EV_TX, 0xFE, 0, 0, 0});  // feed-done marker (gap report)
  }
  this->paced_k_ = k;
}

void IRAM_ATTR NibeBus::rx_byte_(uint8_t b, uint32_t ts, uint8_t flags) {
  (void) flags;  // ninth-bit fact belongs to the transport; this FSM is position-based
  // intra-frame byte-gap tracking (diagnostics): body bytes only — the leading gap belongs to the exchange
  if (this->st_ >= State::S_F_SENDER_HI && this->st_ <= State::S_F_CHK) {
    uint32_t d = ts - this->last_byte_ts_;
    if (d > this->f_max_gap_us_)
      this->f_max_gap_us_ = d;
  }
  this->last_byte_ts_ = ts;

  switch (this->st_) {
    case S_BOUNDARY:
      this->at_boundary_(b, ts);
      break;
    case S_POLL_HI:
      this->at_poll_lo_(b, ts);
      break;
    case S_CTRL:
      this->at_ctrl_(b, ts);
      break;
    case S_F_SENDER_HI:
      this->sender_ = ((uint16_t) b) << 8;
      this->xor_run_ ^= b;
      this->st_ = S_F_SENDER_LO;
      break;
    case S_F_SENDER_LO:
      this->sender_ |= b;
      this->xor_run_ ^= b;
      this->st_ = S_F_LEN;
      break;
    case S_F_LEN:
      this->f_len_ = b;
      this->f_idx_ = 0;
      this->xor_run_ ^= b;
      if (b > FRAME_MAX_DATA) {  // exactly the len f_data_ buffers; real frames stay under 32 (rows ~25, sensor blocks ~19). A larger declared len would over-read f_data_ at the checksum anyway
        this->anomalies_++;
        this->push_event_(EventT{ts, EV_ANOMALY, AN_FRAME_ABORT, this->sender_, b, 0});
        this->st_ = S_HUNT;
      } else if (b == 0) {
        this->st_ = S_F_CHK;
      } else {
        this->st_ = S_F_DATA;
      }
      break;
    case S_F_DATA:
      if (this->f_idx_ < FRAME_MAX_DATA)
        this->f_data_[this->f_idx_] = b;
      this->f_idx_++;
      this->xor_run_ ^= b;
      if (this->f_idx_ >= this->f_len_)
        this->st_ = S_F_CHK;
      break;
    case S_F_CHK:
      this->at_chk_(b, ts);
      break;
    case S_POST_FRAME:
      this->at_post_frame_(b, ts);
      break;
    case S_HUNT:
      if (b == CTRL_ETX) {
        this->push_event_(EventT{ts, EV_CTRL, b, 0, 0, 0});
        this->st_ = S_BOUNDARY;
      }
      break;
  }
}

/// Boundary byte dispatch: poll-pair hi candidates, stray control bytes, or a frame start.
void IRAM_ATTR NibeBus::at_boundary_(uint8_t b, uint32_t ts) {
  if (b == 0x00 || b == 0x41) {  // poll hi: nodes are 00xx (VVM accessories/display), F2025 = 41xx
    this->poll_hi_ = b;
    this->st_ = S_POLL_HI;
  } else if (b == CTRL_ACK) {
    this->push_event_(EventT{ts, EV_CTRL, b, 0, 0, 0});
    if (this->enq_pending_ && this->emulating()) {
      // only a poll pair revokes the grant: this ACK (across a gap) is ours — deliver through the grant window
      this->arm_reply_(RP_FRAME, 0, ts, GRANT_FRAME_DELAY_US);
      this->enq_pending_ = false;
    } else if (this->we_own_close_ && this->emulating() && this->bus_ != nullptr) {
      // sender-closes: this ACK is the master's receipt for OUR frame — we owe the ETX ~+670 us later
      this->arm_reply_(RP_CTRL, CTRL_ETX, ts, FRAME_CLOSE_DELAY_US);
      this->we_own_close_ = false;  // the close is armed/fired — exactly once
    }
  } else if (b == CTRL_ENQ || b == CTRL_NAK || b == CTRL_ETX) {
    this->push_event_(EventT{ts, EV_CTRL, b, 0, 0, 0});
  } else {
    this->begin_frame_(b);
  }
}

/// Second byte of a poll pair: the polled slot opens. Emulator replies right here.
void IRAM_ATTR NibeBus::at_poll_lo_(uint8_t b, uint32_t ts) {
  uint16_t addr = ((uint16_t) this->poll_hi_ << 8) | b;
  this->polled_addr_ = addr;
  this->slot_ours_ = (addr == this->my_addr_);
  this->enq_pending_ = false;  // a new exchange opened; the old grant is void
  // any poll pair voids an in-flight paced feed: another exchange opened — stale bytes must not spray
  this->paced_k_ = 0;
  this->paced_n_ = 0;

  this->push_event_(EventT{ts, EV_POLL, 0, addr, 0, 0});
  if (addr == this->my_addr_) {
    this->polls_own_++;
    this->we_own_close_ = false;  // the master moved on; no close is owed
    if (this->emulating()) {
      // ENQ while we owe the master a frame, plain ACK otherwise — the tx queue is the single source of truth
      uint32_t h = q_load_(&this->txq_head_);
      uint32_t t = q_load_(&this->txq_tail_);
      if (this->rows_seen_ != 0) {
        // first content row: PEEK past expired requests (purge_stale_requests_ owns the pop); keys survive by data[0]
        while (h != t && tx_is_request_(this->tx_queue_[t % TX_QUEUE_N]))
          t = (t + 1) % (2 * TX_QUEUE_N);
      }
      bool have_real = false;  // non-request frame queued (key / menu reply)
      for (uint32_t i = t; i != h; i = (i + 1) % (2 * TX_QUEUE_N))
        if (!tx_is_request_(this->tx_queue_[i % TX_QUEUE_N]))
          have_real = true;
      uint8_t reply;
      if (h == t) {
        reply = CTRL_ACK;  // nothing deliverable
        this->enq_streak_ = 0;
        this->bare_owed_ = 0;
      } else if (have_real) {
        reply = CTRL_ENQ;  // real answers are never throttled (don't starve)
        this->enq_streak_ = 0;
        this->bare_owed_ = 0;
      } else if (this->bare_owed_ > 0) {
        reply = CTRL_ACK;  // breath poll: requests stay queued, the master
        this->bare_owed_--;    // pushes content into the free slots (hypotheses.md 1)
      } else if (this->enq_streak_ >= ANNOUNCE_BURST) {
        reply = CTRL_ACK;  // burst spent: open bare slots for the master
        this->enq_streak_ = 0;
        this->bare_owed_ = ANNOUNCE_BREATH - 1;
      } else {
        reply = CTRL_ENQ;
        this->enq_streak_++;
      }
      // mute-turnaround: the reply tick fires the answer into the master's listening window (+0.8-2.4 ms; protocol.md)
      this->arm_reply_(RP_CTRL, reply, ts, POLL_REPLY_DELAY_US);
      // no self-close: the MASTER closes idle exchanges — our self-close collided with its close and corrupted the bus
    }
  } else {
    this->polls_other_++;
  }
  this->st_ = S_CTRL;
}

/// Response/continuation slot of the just-polled exchange.
void IRAM_ATTR NibeBus::at_ctrl_(uint8_t b, uint32_t ts) {
  if (this->replied_ && b == this->reply_byte_ && (ts - this->reply_ts_) < 4 * this->byte_us_) {
    // our own reply leaking through the transceiver/isolator — hardware mutes most of these
    this->echo_filtered_++;
    return;
  }
  if (b == CTRL_ACK) {
    this->push_event_(EventT{ts, EV_CTRL, b, this->polled_addr_, 0, 0});
    if (this->enq_pending_ && this->slot_ours_) {
      // grant -> frames start ~+160 us (GRANT_FRAME_DELAY_US) — the queue head is taken, never retried
      this->arm_reply_(RP_FRAME, 0, ts, GRANT_FRAME_DELAY_US);
      this->enq_pending_ = false;  // the pending fire owns the delivery now
    }
    this->replied_ = false;
    this->st_ = S_BOUNDARY;
  } else if (b == CTRL_ENQ || b == CTRL_NAK || b == CTRL_ETX) {
    this->push_event_(EventT{ts, EV_CTRL, b, this->polled_addr_, 0, 0});
    this->replied_ = false;
    this->st_ = S_BOUNDARY;
  } else if (b == 0x00 || b == 0x41) {
    // node silent; master moved straight to the next poll
    this->replied_ = false;
    this->poll_hi_ = b;
    this->st_ = S_POLL_HI;
  } else {
    // a frame arrives into the slot (rows after our ACK) or from a node after its ENQ/grant
    this->replied_ = false;
    this->begin_frame_(b);
  }
}

void IRAM_ATTR NibeBus::begin_frame_(uint8_t cmd) {
  this->cmd_ = cmd;
  this->xor_run_ = cmd;
  this->f_max_gap_us_ = 0;       // fresh intra-frame spacing measurement
  this->st_ = S_F_SENDER_HI;
}

void IRAM_ATTR NibeBus::at_chk_(uint8_t b, uint32_t ts) {
  if (b != this->xor_run_ && this->chk_rescue_) {
    // len-lie rescue: some frames carry one byte more than len declares (0x71) — absorb, retest chk, once per frame
    this->chk_rescue_ = false;
    if (this->f_idx_ < FRAME_MAX_DATA) {
      this->f_data_[this->f_idx_++] = b;
      this->f_len_++;
      this->xor_run_ ^= b;
    }
    this->push_event_(EventT{ts, EV_CTRL, 0x00, 0, 0, 1});  // marker: len was off-by-one
    return;  // stay in S_F_CHK — the next byte is the new chk candidate
  }
  this->chk_rescue_ = true;
  if (b != this->xor_run_) {
    this->frames_bad_++;
    this->anomalies_++;
    this->push_event_(EventT{ts, EV_FRAME_BAD, this->cmd_, this->sender_, this->f_len_, 0});
    this->push_event_(EventT{ts, EV_ANOMALY, AN_DESYNC_HUNT, 0, 0, 0});
    this->st_ = S_HUNT;
    return;
  }
  this->frames_ok_++;
  this->f_gap_report_us_ = this->f_max_gap_us_;  // task side reads this per drain
  uint16_t arena = 0xFFFF;
  size_t n = this->f_idx_;
  if (n > 0) {  // single producer (ISR) apart from the event-ring publication
    uint32_t off = this->arena_head_ % ARENA_CAP;
    if (off + n > ARENA_CAP)
      off = 0;  // wrap to the front; the tail may be lost to a slow consumer (counted)
    memcpy(&this->arena_[off], this->f_data_, n);
    this->arena_head_ = off + n;
    arena = (uint16_t) off;
  }
  this->push_event_(EventT{ts, EV_FRAME, this->cmd_, this->sender_, this->f_len_, arena,
                           (uint8_t) (this->slot_ours_ ? EVFL_IN_OUR_SLOT : 0)});
  this->st_ = S_POST_FRAME;

  // push receipts fire late, when quiet (>= PUSH_ACK_DELAY_US; protocol.md) — earlier ACKs collide with bursts
  if (this->slot_ours_ && this->emulating() && this->bus_ != nullptr) {
    // wr/rd SPSC: one writer per counter (ISR arms, loop fires) — a shared head+count pair would tear
    uint8_t rd = __atomic_load_n(&this->push_ack_rd_, __ATOMIC_ACQUIRE);
    uint8_t wr = __atomic_load_n(&this->push_ack_wr_, __ATOMIC_RELAXED);
    if ((uint8_t) (wr - rd) < 4) {
      this->push_acks_[wr % 4] = ts;
      __atomic_store_n(&this->push_ack_wr_, (uint8_t) (wr + 1), __ATOMIC_RELEASE);
    } else {
      this->anomalies_++;  // more pushes than the panel would queue receipts for
      this->push_event_(EventT{ts, EV_ANOMALY, AN_STRAY_CTRL, 0, 0, 0});
    }
  }

}

/// Frame is on the wire; expect the sender's ETX (an ACK may interleave).
void IRAM_ATTR NibeBus::at_post_frame_(uint8_t b, uint32_t ts) {
  if (b == CTRL_ETX || b == CTRL_ENQ) {
    // ETX closes the exchange; an ENQ from the polled node answers a master push and re-arms the parse
    this->push_event_(EventT{ts, EV_CTRL, b, 0, 0, 0});
    this->st_ = S_BOUNDARY;
  } else if (b == CTRL_ACK || b == CTRL_NAK) {
    this->push_event_(EventT{ts, EV_CTRL, b, 0, 0, 0});
  } else if (b == 0x00 || b == 0x41) {
    // missing ETX; salvage by treating the pair as the next poll and move on
    this->anomalies_++;
    this->push_event_(EventT{ts, EV_ANOMALY, AN_STRAY_CTRL, 0, 0, 0});
    this->poll_hi_ = b;
    this->st_ = S_POLL_HI;
  } else {
    this->anomalies_++;
    this->push_event_(EventT{ts, EV_ANOMALY, AN_DESYNC_HUNT, 0, 0, 0});
    this->st_ = S_HUNT;
  }
}

void IRAM_ATTR NibeBus::gap_(uint32_t ts) {
  uint32_t len = ts - this->last_byte_ts_;
  this->push_event_(EventT{ts, EV_GAP, 0, (uint16_t) (len > 0xFFFF ? 0xFFFF : len), 0, 0});
  this->st_ = S_BOUNDARY;
  this->replied_ = false;
  // enq_pending_ SURVIVES the gap: masters may grant an ENQ milliseconds later
}

// ------------------------------ task side ------------------------------

#ifdef USE_ESP32
void IRAM_ATTR NibeBus::poll_reply_tick_(void *arg) {
  // 125 us IRAM tick: paced frame bytes + delayed replies into the master's listening window
  auto *self = static_cast<NibeBus *>(arg);
  uint32_t now = (uint32_t) esp_timer_get_time();
  self->feed_paced_(now);
  self->fire_pending_reply_(now);
}
#endif  // USE_ESP32

void IRAM_ATTR NibeBus::arm_reply_(uint8_t kind, uint8_t ctrl_var, uint32_t ts, uint32_t delay_us) {
  // publish order: payload first, fire_at last — a re-arm before the fire replaces the pending
  this->reply_kind_ = kind;
  this->reply_ctrl_var_ = ctrl_var;
  this->reply_delay_var_ = delay_us;  // the quiet-guard compares against this
  q_store_(&this->reply_fire_at_us_, ts + delay_us);
}

// consumer-side request purge: pops what at_poll_lo_ only peeks (txq_tail_ stays single-writer: tick/loop only)
void IRAM_ATTR NibeBus::purge_stale_requests_() {
  if (!this->emulating() || this->rows_seen_ == 0)
    return;
  uint32_t h = q_load_(&this->txq_head_);
  uint32_t t = q_load_(&this->txq_tail_);
  uint32_t t0 = t;
  while (h != t && tx_is_request_(this->tx_queue_[t % TX_QUEUE_N]))
    t = (t + 1) % (2 * TX_QUEUE_N);
  if (t != t0)
    q_store_(&this->txq_tail_, t);  // requests expire at the first content row
}

void IRAM_ATTR NibeBus::fire_pending_reply_(uint32_t now) {
  this->purge_stale_requests_();
  uint32_t at = q_load_(&this->reply_fire_at_us_);
  if (at == 0 || (int32_t) (now - at) < 0)
    return;
  // CAS claim (tick vs loop callers): payload snapshotted before the claim stays paired to it; a failed CAS is not ours
  const uint8_t kind = this->reply_kind_;
  const uint8_t ctrl_var = this->reply_ctrl_var_;
  const uint32_t delay_var = this->reply_delay_var_;
  if (__sync_val_compare_and_swap(&this->reply_fire_at_us_, at, 0u) != at)
    return;
  if (!this->emulating()) {
    this->enq_pending_ = false;
    return;  // kill switch between arm and fire: transmit nothing
  }
  // quiet-line guard: anything RX'd inside the arming window means the exchange moved on — drop the reply
  if (this->bus_ != nullptr && this->bus_->rx_silence_us() < delay_var) {
    this->reply_drops_++;
    return;  // RP_FRAME: the queue head stays queued for the next grant
  }
  // never interleave a reply with our own paced frame in flight
  if (this->paced_k_ < this->paced_n_) {
    this->reply_drops_++;
    return;
  }
  if (kind == RP_CTRL) {
    if (this->reply_to_poll_(now, ctrl_var)) {
      this->replied_ = true;  // arm the echo discriminator at the real TX moment
      this->reply_byte_ = ctrl_var;
      this->reply_ts_ = now;
    }
  } else if (kind == RP_FRAME) {
    this->deliver_frame_(now);  // pops+sends the queue head (never retried)
  }
}

void NibeBus::setup() {
  if (this->emulator_intent_ && !this->transmit_capable_())
    ESP_LOGE(TAG, "role emulator requires rs485_bus 'active: true' — refusing to transmit");
#ifndef USE_SWITCH
  // switchless builds (host harness): arm at setup and start the request cycle — the panel's power-up behavior
  if (this->emulator_intent_ && this->transmit_capable_()) {
    this->armed_ = true;
    this->trigger_announce();
    ESP_LOGW(TAG, "no arm switch in this build: emulator armed at setup, requesting content");
  }
#endif
  if (this->bus_ != nullptr)
    this->byte_us_ = 11u * (1000000u / this->bus_->get_baud_rate());  // 9-bit chars + stop
  ESP_LOGCONFIG(TAG, "NibeBus: %s role, address %04X", this->role_name(), this->my_addr_);
#ifdef USE_ESP32
  if (!this->emulator_intent_ || !this->transmit_capable_())
    return;  // sniffer builds never pace replies: skip the 125 us tick entirely
  if (this->reply_timer_ == nullptr) {
    esp_timer_create_args_t targs = {};
    targs.callback = &NibeBus::poll_reply_tick_;
    targs.arg = this;
    targs.name = "nibe_reply";
    targs.skip_unhandled_events = true;
    if (esp_timer_create(&targs, &this->reply_timer_) == ESP_OK)
      esp_timer_start_periodic(this->reply_timer_, 125);
    else
      ESP_LOGE(TAG, "poll-reply timer create failed");
  }
#endif
}

bool NibeBus::queue_frame_(uint8_t cmd, const uint8_t *data, size_t len) {
  uint8_t buf[TX_FRAME_MAX];
  size_t n = build_frame(buf, sizeof(buf), cmd, this->my_addr_, data, len);
  if (n == 0 || n > TX_FRAME_MAX - 1)
    return false;
  uint32_t head = q_load_(&this->txq_head_);
  uint32_t tail = q_load_(&this->txq_tail_);
  uint32_t used = (head + 2 * TX_QUEUE_N - tail) % (2 * TX_QUEUE_N);
  if (used >= TX_QUEUE_N) {  // 8 slots over mod-16 indices: count, never head==tail-1
    ESP_LOGW(TAG, "outbound queue full; frame %02X dropped", cmd);
    return false;
  }
  uint32_t next = (head + 1) % (2 * TX_QUEUE_N);
  PendingTx &slot = this->tx_queue_[head % TX_QUEUE_N];
  slot.len = (uint8_t) n;
  memcpy(slot.buf, buf, n);
  q_store_(&this->txq_head_, next);
  return true;
}

void NibeBus::pump_announces_() {
  // request pump: "40 <sys> 00 00" every poll until the FIRST content row (protocol.md "The display slot")
  if (!this->emulating() || !this->announce_pump_active_)
    return;
  if (this->rows_seen_ != 0) {
    this->announce_pump_active_ = 0;
    return;
  }
  // requests pause while a real answer (key, menu reply) is queued — the display's own answer goes first
  uint32_t h = q_load_(&this->txq_head_);
  uint32_t t = q_load_(&this->txq_tail_);
  for (uint32_t i = t; i != h; i = (i + 1) % (2 * TX_QUEUE_N))
    if (!tx_is_request_(this->tx_queue_[i % TX_QUEUE_N]))
      return;
  uint8_t data[4] = {0x40, this->request_byte_, 0x00, 0x00};  // byte[1]: relaid system value (queue_key matches)
  // fill to half the queue: continuous requests, slots stay free for keys/menu replies
  for (;;) {
    h = q_load_(&this->txq_head_);
    t = q_load_(&this->txq_tail_);
    uint32_t used = (h - t + 2 * TX_QUEUE_N) % (2 * TX_QUEUE_N);
    if (used >= TX_QUEUE_N / 2)
      break;
    if (!this->queue_frame_(CMD_ANNOUNCE, data, 4))
      break;
  }
}

bool NibeBus::queue_key(uint8_t key) {
  return this->queue_key_number(key, this->request_byte_);
}

bool NibeBus::queue_key_number(uint8_t key, uint8_t value) {
  if (!this->emulating()) {
    ESP_LOGW(TAG, "role is %s: key 0x%02X val 0x%02X not queued (refuses to transmit)", this->role_name(), key, value);
    return false;
  }
  uint8_t data[4] = {key, value, 0, 0};
  return this->queue_frame_(CMD_KEY, data, 4);
}

bool NibeBus::queue_register_read() {
  if (!this->emulating()) {
    ESP_LOGW(TAG, "role is %s: register read 05 %02X not queued (refuses to transmit)",
             this->role_name(), this->register_addr_);
    return false;
  }
  uint8_t data[2] = {REG_PREFIX, this->register_addr_};
  ESP_LOGI(TAG, "queueing register read 0x72: 05 %02X (answer lands in the frame log)", this->register_addr_);
  return this->queue_frame_(CMD_REG_READ, data, sizeof(data));
}

bool NibeBus::trigger_announce() {
  if (!this->emulating())
    return false;
  // fresh request cycle after re-arm: the request-repeat pump re-runs until the first content row
  this->rows_seen_ = 0;
  this->announce_pump_active_ = 1;
  this->enq_streak_ = 0;
  this->bare_owed_ = 0;
  this->pump_announces_();
  return true;
}

// one engine event (task context): logging, entity updates, loop-latency-OK emulator transitions
void NibeBus::process_event_(const EventT &ev) {
  // wire stamps print with true wire deltas; loop-synthetic events carry ts=0
  char tail[28];
  if (ev.ts != 0) {
    uint32_t d = ev.ts - this->last_ev_ts_;  // unsigned wrap-safety across rolls
    snprintf(tail, sizeof(tail), " @%lu (+%lu)", (unsigned long) ev.ts, (unsigned long) d);
    this->last_ev_ts_ = ev.ts;
  } else {
    tail[0] = 0;  // synthetic loop-time events (no wire moment)
  }
  switch (ev.type) {
    case EV_POLL: {
      if (ev.x == this->my_addr_) {
        this->last_own_poll_ms_ = esphome::millis();  // loop-clock bookkeeping
        ESP_LOGD(TAG, "poll %04X (own)%s", ev.x, tail);
      } else if (this->log_level_ >= 2) {
        ESP_LOGD(TAG, "poll %04X%s", ev.x, tail);
      }
      break;
    }
    case EV_CTRL: {
      if (this->log_level_ >= 2)
        ESP_LOGD(TAG, "ctrl %02X (slot %04X)%s", ev.a, ev.x, tail);
      break;
    }
    case EV_TX: {
      if (ev.a == 0xFE && ev.len == 0) {
        // paced feed done: report the achieved inter-byte cadence
        ESP_LOGI(TAG, "tx feed done: gaps %lu-%lu us%s",
                 (unsigned long) this->paced_gap_min_, (unsigned long) this->paced_gap_max_, tail);
        break;
      }
      ESP_LOGI(TAG, "tx: %02X len=%u%s", ev.a, ev.len, tail);
      if (ev.len > 1) {
        // same format as received frames — our own sent frame, byte-for-byte
        uint8_t fbuf[TX_FRAME_MAX];
        uint8_t n = this->tx_log_len_ < TX_FRAME_MAX ? this->tx_log_len_ : TX_FRAME_MAX;
        for (uint8_t i = 0; i < n; i++)  // stack copy off the volatile mirror
          fbuf[i] = this->tx_log_buf_[i];
        const uint8_t *fr = fbuf;
        if (n >= 5 && fr[0] == ev.a) {
          char hexbuf[3 * 32 + 1];
          hexbuf[0] = 0;
          if (fr[3] > 0 && fr[3] <= 32)
            hex_(hexbuf, sizeof(hexbuf), &fr[4], fr[3]);
          ESP_LOGI(TAG, "  tx frame cmd=%02X sender=%02X%02X len=%u data=%s chk=%02X",
                   fr[0], fr[1], fr[2], (unsigned) fr[3], hexbuf, fr[4 + fr[3]]);
        }
      }
      if (ev.a == CMD_ANNOUNCE)
        this->pump_announces_();
      break;
    }
    case EV_GAP: {
      this->last_gap_us_ = ev.x;
      if (this->log_level_ >= 2)
        ESP_LOGD(TAG, "gap %u us%s", ev.x, tail);
      break;
    }
    case EV_ANOMALY: {
      const char *why = "?";
      switch (ev.a) {
        case AN_DESYNC_HUNT: why = "checksum failed -> hunting ETX"; break;
        case AN_FRAME_ABORT: why = "frame len absurd"; break;
        case AN_ECHO_FILTERED: why = "echo filtered"; break;
        case AN_TX_REFUSED: why = "tx refused (dropped, NOT retried)"; break;
        case AN_OWN_POLL_TIMEOUT: why = "master stopped polling us"; break;
        case AN_STRAY_CTRL: why = "control byte/reply missing somewhere"; break;
      }
      ESP_LOGW(TAG, "anomaly: %s", why);
      if ((ev.a == AN_DESYNC_HUNT || ev.a == AN_FRAME_ABORT || ev.a == AN_STRAY_CTRL) &&
          this->bus_ != nullptr)
        this->bus_->debug_dump_recent();
      break;
    }
    case EV_FRAME_BAD: {
      ESP_LOGW(TAG, "BAD frame cmd=%02X sender=%04X len=%u%s", ev.a, ev.x, ev.len, tail);
      break;
    }
    case EV_FRAME: {
      uint8_t payload[FRAME_MAX_DATA];
      uint8_t n = 0;
      if (ev.arena != 0xFFFF && ev.len > 0) {
        n = (uint8_t) std::min((size_t) ev.len, (size_t) FRAME_MAX_DATA);
        memcpy(payload, &this->arena_[ev.arena], n);
      }
      char hexbuf[3 * FRAME_MAX_DATA + 1];
      hexbuf[0] = 0;
      hex_(hexbuf, sizeof(hexbuf), payload, n);
      if (this->log_level_ >= 1) {
        ESP_LOGD(TAG, "frame cmd=%02X sender=%04X len=%u data=%s%s", ev.a, ev.x, (unsigned) ev.len, hexbuf, tail);
        if (this->log_level_ >= 2)
          ESP_LOGD(TAG, "  intra-byte gap max: %lu us", (unsigned long) this->f_gap_report_us_);
      }

      // display rows ONLY in our own slot — a probe must not count the panel's rows (dead-arms the pump)
      if (ev.x == ADDR_VVM300 && (ev.flags & EVFL_IN_OUR_SLOT) &&
          ev.a >= CMD_ROW_BASE && ev.a <= CMD_ROW_BASE + 3) {
        uint8_t row = ev.a - CMD_ROW_BASE + 1;
        if (row >= 2) {  // rows 2..4: latin-1 text, NUL padded
          bool first_content = (this->rows_seen_ == 0);
          this->rows_seen_ |= (uint8_t) (1u << (row - 2));
          if (first_content) {
            // the repeat phase ends at the FIRST content row (protocol.md "The display slot")
            this->announce_pump_active_ = 0;  // queued requests pop at the next poll
            ESP_LOGI(TAG, "first content row (row %u) received: request pump off", row);
          }
          if (this->rows_complete_()) {
            ESP_LOGI(TAG, "rows 2-4 received: display content complete");
          }
          std::string text;
          text.reserve(n + 4);
          for (int i = 0; i < n && payload[i] != 0; i++) {
            uint8_t c = payload[i];
            if (c < 0x80) {
              text.push_back((char) c);
            } else {  // latin-1 -> utf-8
              text.push_back((char) (0xC0 | (c >> 6)));
              text.push_back((char) (0x80 | (c & 0x3F)));
            }
          }
#ifdef USE_NIBE_DISPLAY_WEB
          strlcpy(this->snap_rows_[row - 2], text.c_str(), sizeof(this->snap_rows_[0]));
#endif
#ifdef USE_TEXT_SENSOR
          for (const auto &sink : this->row_sinks_)
            if (sink.row == row)
              sink.sensor->publish_state(text);
#endif
        } else {  // row 1: binary symbol strip (undocumented bits; publish raw hex for now)
#ifdef USE_NIBE_DISPLAY_WEB
          this->sym_raw_len_ = std::min((int) n, (int) sizeof(this->sym_raw_));
          memcpy(this->sym_raw_, payload, this->sym_raw_len_);
#endif
#ifdef USE_TEXT_SENSOR
          for (const auto &sink : this->row_sinks_)
            if (sink.row == 1)
              sink.sensor->publish_state(hexbuf);
#endif
        }
      }

      // F2025 sensor block: passive decode only, we are not part of that exchange (commands.md)
      if (ev.x == ADDR_F2025 && ev.a == CMD_SENSOR) {
#ifdef USE_TEXT_SENSOR
        for (const auto &sink : this->f2025_word_hex_sinks_) {
          size_t need = ((size_t) sink.first + 1) * 2;
          if ((size_t) n >= need) {
            uint16_t raw = (uint16_t) payload[2 * sink.first] | ((uint16_t) payload[2 * sink.first + 1] << 8);
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%04X", (unsigned) raw);
            sink.second->publish_state(hex);
          }
        }
#endif
        if (this->log_level_ >= 1)
          ESP_LOGD(TAG, "F2025 sensor block (word0..4: %.1f %.1f %.1f %.1f %.1f)",
                   n >= 2 ? ((int16_t)(payload[0] | (payload[1] << 8))) / 10.0f : 0.0f,
                   n >= 4 ? ((int16_t)(payload[2] | (payload[3] << 8))) / 10.0f : 0.0f,
                   n >= 6 ? ((int16_t)(payload[4] | (payload[5] << 8))) / 10.0f : 0.0f,
                   n >= 8 ? ((int16_t)(payload[6] | (payload[7] << 8))) / 10.0f : 0.0f,
                   n >= 10 ? ((int16_t)(payload[8] | (payload[9] << 8))) / 10.0f : 0.0f);
      }

#ifdef USE_SENSOR
      // generic F2025 answer register: u16/s16 LE at a byte offset, scaled (commands.md)
      if (ev.x == ADDR_F2025) {
        for (const auto &sink : this->f2025_reg_sinks_) {
          if (sink.first.cmd != ev.a)
            continue;
          size_t need = (size_t) sink.first.offset + 2;
          if ((size_t) n >= need) {
            uint16_t raw = (uint16_t) payload[sink.first.offset] | ((uint16_t) payload[sink.first.offset + 1] << 8);
            float value = sink.first.is_signed ? (float) (int16_t) raw : (float) raw;
            sink.second->publish_state(value * sink.first.scale);
          }
        }
      }
#endif

#ifdef USE_TEXT_SENSOR
      // F2025 frame hex for the decoding watch (commands.md)
      if (ev.x == ADDR_F2025) {
        for (const auto &sink : this->f2025_frame_hex_sinks_)
          if (sink.first == ev.a)
            sink.second->publish_state(hexbuf);
      }
#endif

      // -- our address emitting = panel traffic (physical panel, or our own frames echoed) --
      uint16_t key_src = this->my_addr_;
      // key frames: data[0] discriminates; data[1] is the relayed system value (commands.md)
      if (ev.x == key_src && ev.a == CMD_KEY && ev.len == 4 && n >= 2 && payload[0] <= 4) {
        uint8_t key = payload[0];
        const char *name = key <= 4 ? KEY_NAMES[key] : "?";
        ESP_LOGI(TAG, "key frame: 0x%02X (%s)", key, name);
#ifdef USE_TEXT_SENSOR
        if (this->key_name_sink_ != nullptr)
          this->key_name_sink_->publish_state(name);
#endif
      }
      if (ev.x == this->my_addr_ && ev.a == CMD_ANNOUNCE && ev.len == 4 && n >= 2 && payload[0] == 0x40) {
        ESP_LOGI(TAG, "display request frame from %04X (relay byte: %02X)", ev.x, payload[1]);
      }

      // 0x22 is the master->F2025 boot link check; announces are self-initiated (hypotheses.md)
      if (this->emulating() && ev.x == ADDR_VVM300 && (ev.flags & EVFL_IN_OUR_SLOT) &&
          ev.a == CMD_MENU2 && this->menu_reply_) {
        ESP_LOGI(TAG, "master 0x62 menu request: queueing reply");
        uint8_t data[1] = {0x0F};
        this->queue_frame_(CMD_MENU2, data, 1);
      }

      // clock query: 7-byte BCD + day-of-week reply; opt-in — it sets the master's clock (protocol.md)
      if (this->emulating() && this->d4_reply_ && ev.x == ADDR_VVM300 && (ev.flags & EVFL_IN_OUR_SLOT) &&
          ev.a == CMD_CLOCK) {
#ifdef USE_TIME
        esphome::ESPTime t = this->time_source_ != nullptr ? this->time_source_->now() : esphome::ESPTime();
#else
        // no time platform: host epoch is real time, a bare ESP starts at 1970 — is_valid() silences that
        esphome::ESPTime t = esphome::ESPTime::from_epoch_local(::time(nullptr));
#endif
        if (!t.is_valid()) {
          ESP_LOGW(TAG, "master 0xD4 clock query: our clock is not synced/valid — not answering");
        } else {
          uint8_t (*bcd)(uint8_t) = [](uint8_t v) -> uint8_t { return (uint8_t) (((v / 10) << 4) | (v % 10)); };
          // tail = day-of-week, Sunday=1 (protocol.md); d4_tail overrides, 0 = auto
          uint8_t tail = this->d4_tail_ != 0 ? this->d4_tail_ : t.day_of_week;
          uint8_t data[7] = {bcd((uint8_t) (t.year % 100)), bcd((uint8_t) t.month), bcd((uint8_t) t.day_of_month),
                             bcd((uint8_t) t.hour), bcd((uint8_t) t.minute), bcd((uint8_t) t.second), tail};
          ESP_LOGI(TAG, "master 0xD4 clock query: queueing %02X %02X %02X %02X %02X %02X %02X "
                        "(%04u-%02u-%02u %02u:%02u:%02u)",
                   data[0], data[1], data[2], data[3], data[4], data[5], data[6], (unsigned) t.year,
                   (unsigned) t.month, (unsigned) t.day_of_month, (unsigned) t.hour, (unsigned) t.minute,
                   (unsigned) t.second);
          this->queue_frame_(CMD_CLOCK, data, 7);
        }
      }

      break;
    }
  }
}

void NibeBus::emulator_push_ack_tick_() {
  // fire ONE aged push receipt per pass, only when the line is quiet (protocol.md)
  if (!this->emulating() || this->bus_ == nullptr)
    return;
  uint8_t rd = __atomic_load_n(&this->push_ack_rd_, __ATOMIC_RELAXED);
  uint8_t wr = __atomic_load_n(&this->push_ack_wr_, __ATOMIC_ACQUIRE);
  if ((uint8_t) (wr - rd) == 0)
    return;
  if (this->bus_->rx_silence_us() < 2500)  // mid-burst: never collide with the stream
    return;
  uint32_t now = (uint32_t) esphome::micros();
  uint32_t arm_ts = __atomic_load_n(&this->push_acks_[rd % 4], __ATOMIC_RELAXED);
  if ((now - arm_ts) < PUSH_ACK_DELAY_US)  // render lag not elapsed yet
    return;
  __atomic_store_n(&this->push_ack_rd_, (uint8_t) (rd + 1), __ATOMIC_RELEASE);  // pop: refused sends still drop, never retried
  uint8_t ack = CTRL_ACK;  // stack copy (kept off flash on every path, as usual)
  if (this->bus_->send_burst(&ack, 1, false)) {
    this->tx_sent_++;
    this->push_event_(EventT{now, EV_TX, CTRL_ACK, 0, 0, 0});
  } else {
    this->tx_refused_++;
  }
}

void NibeBus::loop() {
  uint32_t h = q_load_(&this->ev_head_);
  uint32_t t = q_load_(&this->ev_tail_);
  while (t != h) {
    this->process_event_(this->events_[t]);
    t = (t + 1) % EV_RING_N;
  }
  q_store_(&this->ev_tail_, t);

  // top up the request queue from loop too: row pushes produce no EV_TX to refill from
  this->pump_announces_();

  // mute-turnaround dispatch: device = the 125 us esp_timer tick; loop() is the HOST tick only
  if (this->emulator_intent_ && this->bus_ != nullptr) {
#ifndef USE_ESP32
    this->feed_paced_(this->bus_->now_us());  // host builds: loop is the feeder
    this->fire_pending_reply_(this->bus_->now_us());  // host builds: loop is the sole firer
#endif
  }

  uint32_t now = esphome::millis();

  // display emulator: trailing push receipts (protocol.md)
  this->emulator_push_ack_tick_();

#ifdef USE_BINARY_SENSOR
  if (this->polled_sink_ != nullptr) {
    uint32_t last = this->last_own_poll_ms_;
    bool fresh = last != 0 && (now - last) < 5000;
    this->polled_sink_->publish_state(fresh);
  }
#endif

#ifdef USE_NIBE_DISPLAY_WEB
  // virtual display page LAST: rendering is loop-rate work, it never stalls wire timing
  this->serve_display_web();
#endif

  // safety: a master that stopped polling must never receive stale frames queued seconds ago
  if (this->emulating() && this->last_own_poll_ms_ != 0 &&
      now - this->last_own_poll_ms_ > NO_POLL_TIMEOUT_S * 1000) {
    q_store_(&this->txq_tail_, q_load_(&this->txq_head_));
    ESP_LOGW(TAG, "no polls to %04X for %u s: outbound queue dropped", this->my_addr_,
             (unsigned) NO_POLL_TIMEOUT_S);
    this->anomalies_++;
    this->push_event_(EventT{0, EV_ANOMALY, AN_OWN_POLL_TIMEOUT, 0, 0, 0});
    this->last_own_poll_ms_ = now;  // re-arm
  }

  if (this->next_summary_ms_ == 0 || now >= this->next_summary_ms_) {
    this->next_summary_ms_ = now + 10000;
    const auto bs = this->bus_ != nullptr ? this->bus_->stats() : rs485_bus::BusStats{};
    {
      // stack copies: the summary log hit an xtensa GCC ICE with 11 direct volatile args
      const unsigned f_ok = this->frames_ok_, f_bad = this->frames_bad_, p_own = this->polls_own_,
                     p_other = this->polls_other_, txs = this->tx_sent_, txr = this->tx_refused_,
                     rdrop = this->reply_drops_, ech = this->echo_filtered_, anom = this->anomalies_,
                     evov = this->ev_overflow_;
      ESP_LOGI(TAG, "bus: frames ok=%u bad=%u polls_own=%u other=%u tx=%u txref=%u replydrop=%u gaps=%u echoes=%u anomalies=%u ev_drop=%u",
               f_ok, f_bad, p_own, p_other, txs, txr, rdrop, (unsigned) bs.gaps, ech, anom, evov);
      // transport health: framing and parity errors are the corruption signals we keep
      ESP_LOGI(TAG, "bus health: frm_err=%u par_err=%u gl=%u min_rx_delta=%lu us",
               (unsigned) bs.frm_errs, (unsigned) bs.par_errs, (unsigned) bs.glitches,
               (unsigned long) (bs.min_rx_delta_us == ~0u ? 0 : bs.min_rx_delta_us));
    }
#ifdef USE_SENSOR
    for (const auto &sink : this->counter_sinks_) {
      auto *s = sink.second;
      switch (sink.first) {
        case 0: s->publish_state(this->frames_ok_); break;
        case 1: s->publish_state(this->frames_bad_); break;
        case 2: s->publish_state(this->polls_own_); break;
        case 3: s->publish_state(this->polls_other_); break;
        case 4: s->publish_state(this->tx_sent_); break;
        case 5: s->publish_state(this->anomalies_); break;
        case 6: s->publish_state(this->echo_filtered_); break;
        case 7: s->publish_state(this->ev_overflow_); break;
        case 8: s->publish_state(this->bus_ != nullptr ? this->bus_->stats().par_errs : 0); break;
        case 9: s->publish_state(this->bus_ != nullptr ? this->bus_->stats().frm_errs : 0); break;
      }
    }
    if (this->gap_sink_ != nullptr)
      this->gap_sink_->publish_state(this->last_gap_us_ / 1000.0f);
#endif
  }
}

void NibeBus::dump_config() {
  ESP_LOGCONFIG(TAG, "NibeBus:");
  ESP_LOGCONFIG(TAG, "  role: %s, address: %04X", this->role_name(), this->my_addr_);
  ESP_LOGCONFIG(TAG, "  rows 2/3/4 seen: %d/%d/%d, request pump: %s, menu_reply: %s",
                (this->rows_seen_ & 1) ? 1 : 0, (this->rows_seen_ & 2) ? 1 : 0,
                (this->rows_seen_ & 4) ? 1 : 0, this->announce_pump_active_ ? "ON" : "OFF",
                this->menu_reply_ ? "ON" : "OFF");
  if (this->bus_ != nullptr)
    ESP_LOGCONFIG(TAG, "  9-bit line: TX marks ENQ 1 / data 0; RX carries the slot as a per-byte fact");
  ESP_LOGCONFIG(TAG, "  engine timings: poll_reply %lu us, grant_frame %lu us, tx_byte_gap %lu us, "
                "frame_close %lu us, push_ack %lu us, reply_drops %lu",
                (unsigned long) POLL_REPLY_DELAY_US, (unsigned long) GRANT_FRAME_DELAY_US,
                (unsigned long) TX_BYTE_GAP_US, (unsigned long) FRAME_CLOSE_DELAY_US,
                (unsigned long) PUSH_ACK_DELAY_US, (unsigned long) this->reply_drops_);
  ESP_LOGCONFIG(TAG, "  announce governor: burst %u, breath %u (policy; hypotheses.md 1)",
                (unsigned) ANNOUNCE_BURST, (unsigned) ANNOUNCE_BREATH);
  ESP_LOGCONFIG(TAG, "  0xD4 clock answer: %s, tail 0x%02X (0=auto: day-of-week, Sunday=1; "
                "answers only when the clock is valid)",
                this->d4_reply_ ? "ENABLED" : "off", (unsigned) this->d4_tail_);
}

#ifdef USE_SWITCH
void NibeSwitch::write_state(bool state) {
  this->hub_->set_armed(state);
  if (state) {
    // arming starts a fresh request cycle: the master re-detects us within one backoff poll (protocol.md)
    this->hub_->trigger_announce();
  }
  this->publish_state(state);
}
#endif  // USE_SWITCH

#ifdef USE_BUTTON
void NibeButton::press_action() {
  if (this->read_register_) {
    this->hub_->queue_register_read();  // target pinned by the register-address number
  } else if (this->announce_) {
    this->hub_->trigger_announce();
  } else {
    this->hub_->queue_key(this->key_);
  }
}
#endif  // USE_BUTTON

void NibeBus::set_knob_placement(uint8_t position) {
  this->knob_pos_ = position;
  // one knob event per placement change; queue_key_number gates on the emulator role like keys
  this->queue_key_number(KEY_KNOB, position);
}

#ifdef USE_NUMBER
void NibeKnobNumber::setup() {
  this->publish_state(127.0f);  // mid placement: the knob idles vertical
}

void NibeKnobNumber::control(float value) {
  const float lo = this->traits.get_min_value();
  const float hi = this->traits.get_max_value();
  value = value < lo ? lo : (value > hi ? hi : value);
  uint8_t placement = (uint8_t) (long) lroundf(value);
  this->hub_->set_knob_placement(placement);  // also syncs the page dial
  // optimistic echo: the master never reports placements back
  this->publish_state((float) placement);
}

void NibeRegisterNumber::setup() {
  this->publish_state(0.0f);  // probe register 0 until the user picks one
}

void NibeRegisterNumber::control(float value) {
  const float lo = this->traits.get_min_value();
  const float hi = this->traits.get_max_value();
  value = value < lo ? lo : (value > hi ? hi : value);
  uint8_t reg = (uint8_t) (long) lroundf(value);
  this->hub_->set_register_address(reg);
  this->publish_state((float) reg);  // local record; no bus traffic until the read button
}
#endif  // USE_NUMBER

}  // namespace esphome::nibe
