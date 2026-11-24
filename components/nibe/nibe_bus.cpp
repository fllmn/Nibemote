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

/// SPSC ring push: one producer (ISR or the host loop) writes head, one consumer writes tail.
template <typename T, size_t N>
IRAM_ATTR inline bool ring_push_(volatile uint32_t *head, volatile uint32_t *tail, T *ring, const T &rec) {
  uint32_t h = q_load_(head);
  uint32_t next = (h + 1) % N;
  if (next == q_load_(tail))
    return false;
  ring[h] = rec;
  q_store_(head, next);
  return true;
}

static inline void hex_(char *out, size_t cap, const uint8_t *data, size_t len) {
  size_t p = 0;
  for (size_t i = 0; i < len && p + 3 < cap; i++) {
    p += (size_t) snprintf(out + p, cap - p, "%02X ", data[i]);
  }
}

// ------ engine side: ISR ingest + wire mechanics, task parse (ADR-0001) ------

void IRAM_ATTR NibeBus::push_wire_(uint8_t b, uint32_t ts, uint8_t flags) {
  if (!ring_push_<WireRecord, WIRE_RING_N>(&this->wire_head_, &this->wire_tail_, this->wire_,
                                           WireRecord{ts, b, flags})) {
    this->wire_overflow_++;  // parser too slow: drop newest, and resync it on what survived
    this->wire_lost_ = true;
  }
}

void IRAM_ATTR NibeBus::push_wire_gap_(uint32_t ts) {
  if (!ring_push_<WireRecord, WIRE_RING_N>(&this->wire_head_, &this->wire_tail_, this->wire_,
                                           WireRecord{ts, 0, WRF_GAP})) {
    this->wire_overflow_++;
    this->wire_lost_ = true;
  }
}

void IRAM_ATTR NibeBus::push_tx_notice_(uint32_t ts, uint8_t kind, uint8_t byte, uint8_t len) {
  // log only: a full notice ring drops, and the loop pumps the request queue every pass regardless
  ring_push_<TxNotice, TX_NOTICE_N>(&this->tx_notice_head_, &this->tx_notice_tail_, this->tx_notices_,
                                    TxNotice{ts, kind, byte, len});
}

bool IRAM_ATTR NibeBus::reply_to_poll_(uint32_t ts, uint8_t reply) {
  if (!this->emulating() || this->bus_ == nullptr)
    return false;
  if (!this->bus_->send_burst(&reply, 1, reply == CTRL_ENQ)) {
    this->tx_refused_++;
    this->push_tx_notice_(ts, TXN_TX_REFUSED, 0, 0);
    return false;
  }
  this->tx_sent_++;
  this->enq_pending_ = (reply == CTRL_ENQ);
  this->heard_since_tx_ = false;  // the next byte heard may be our own echo
  this->push_tx_notice_(ts, TXN_REPLY, reply, 0);
  return true;
}

bool IRAM_ATTR NibeBus::deliver_frame_(uint32_t ts) {
  uint32_t h = q_load_(&this->txq_head_);
  uint32_t t = q_load_(&this->txq_tail_);
  if ((h - t + 2 * TX_QUEUE_N) % (2 * TX_QUEUE_N) == 0)
    return false;
  const PendingTx &tx = this->tx_queue_[t % TX_QUEUE_N];
  if (tx.len <= 1)  // only whole frames are queued (send builds cmd..chk); the guard cannot fire
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
  this->push_tx_notice_(ts, TXN_FRAME, out[0], (uint8_t) tx.len);
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
      this->push_tx_notice_(now, TXN_FEED_DONE, 0, 0);  // feed-done marker (gap report)
  }
  this->paced_k_ = k;
}

void IRAM_ATTR NibeBus::rx_byte_(uint8_t b, uint32_t ts, uint8_t flags) {
  const bool marked = (flags & rs485_bus::RXF_HB9) != 0;
  // hardware mutes the receiver while transmitting; this filter is belt-and-braces (protocol.md).
  // One-shot: only the FIRST byte heard after our own reply can be its echo.
  const bool first_since_tx = !this->heard_since_tx_;
  this->heard_since_tx_ = true;
  if (first_since_tx && this->ws_ == WS_CTRL && b == this->reply_byte_ &&
      (ts - this->reply_ts_) < 4 * this->byte_us_) {
    this->echo_filtered_++;
    return;
  }
  this->track_byte_(b, ts, marked);
  this->push_wire_(b, ts, marked ? WRF_MARKED : 0);
}

/// Wire mechanics only: who was polled, when a grant/close is ours, where a frame ends. No checksums, no meaning.
void IRAM_ATTR NibeBus::track_byte_(uint8_t b, uint32_t ts, bool marked) {
  switch (this->ws_) {
    case WS_BOUNDARY: {
      if (is_poll_hi(b)) {  // poll hi: nodes are 00xx (VVM accessories/display), F2025 = 41xx
        this->ws_poll_hi_ = b;
        this->ws_ = WS_POLL_HI;
      } else if (b == CTRL_ACK) {
        if (this->enq_pending_ && this->emulating()) {
          // only a poll pair revokes the grant: this ACK (across a gap) is ours — deliver through the window
          this->arm_reply_(RP_FRAME, 0, ts, GRANT_FRAME_DELAY_US);
          this->enq_pending_ = false;
        } else if (this->we_own_close_ && this->emulating()) {
          // sender-closes: this ACK is the master's receipt for OUR frame — we owe the ETX ~+670 us later
          this->arm_reply_(RP_CTRL, CTRL_ETX, ts, FRAME_CLOSE_DELAY_US);
          this->we_own_close_ = false;  // the close is armed/fired — exactly once
        }
      } else if (!is_ctrl_byte(b)) {
        this->ws_ = WS_FRAME_HDR;
        this->ws_skip_ = 2;  // sender hi/lo; the len byte is read by WS_FRAME_LEN
      }
      break;
    }
    case WS_POLL_HI: {
      const uint16_t addr = ((uint16_t) this->ws_poll_hi_ << 8) | b;
      this->slot_ours_ = (addr == this->my_addr_);
      this->enq_pending_ = false;  // a new exchange opened; the old grant is void
      this->paced_k_ = 0;          // any poll pair voids an in-flight paced feed
      this->paced_n_ = 0;
      if (this->slot_ours_)
        this->we_own_close_ = false;  // the master moved on; no close is owed
      this->ws_ = WS_CTRL;
      break;
    }
    case WS_CTRL:
      if (b == CTRL_ACK) {
        if (this->enq_pending_ && this->slot_ours_) {
          // grant -> frames start ~+160 us (GRANT_FRAME_DELAY_US) — the queue head is taken, never retried
          this->arm_reply_(RP_FRAME, 0, ts, GRANT_FRAME_DELAY_US);
          this->enq_pending_ = false;
        }
        this->ws_ = WS_BOUNDARY;
      } else if (is_ctrl_byte(b)) {
        this->ws_ = WS_BOUNDARY;
      } else if (is_poll_hi(b)) {
        this->ws_poll_hi_ = b;  // node silent; the master moved straight to the next poll
        this->ws_ = WS_POLL_HI;
      } else {
        this->ws_ = WS_FRAME_HDR;  // a frame in the slot (rows after our ACK, or a node after its ENQ)
        this->ws_skip_ = 2;
      }
      break;
    case WS_FRAME_HDR:
      if (--this->ws_skip_ == 0)
        this->ws_ = WS_FRAME_LEN;
      break;
    case WS_FRAME_LEN:
      if (b > FRAME_MAX_DATA) {
        this->ws_ = WS_HUNT;
      } else {
        this->ws_cnt_ = (uint16_t) b + 1;  // data + checksum
        this->ws_ = WS_FRAME_BODY;
      }
      break;
    case WS_FRAME_BODY:
      if (marked) {
        // data bytes are unmarked (protocol.md); a marked byte here can only be a genuine boundary
        // token, so treat it as one rather than counting it as payload
        if (is_poll_hi(b)) {
          this->ws_poll_hi_ = b;
          this->ws_ = WS_POLL_HI;
          break;
        }
        if (b == CTRL_ETX || b == CTRL_ENQ) {
          this->ws_ = WS_BOUNDARY;
          break;
        }
      }
      if (this->ws_cnt_ > 0 && --this->ws_cnt_ == 0)
        this->ws_ = WS_POST_FRAME;
      break;
    case WS_POST_FRAME:
      if (b == CTRL_ETX || b == CTRL_ENQ) {
        this->ws_ = WS_BOUNDARY;
      } else if (b == CTRL_ACK || b == CTRL_NAK) {
        // await the sender's ETX
      } else if (is_poll_hi(b)) {
        // missing ETX; salvage by treating the pair as the next poll and move on
        this->ws_poll_hi_ = b;
        this->ws_ = WS_POLL_HI;
      } else {
        this->ws_ = WS_HUNT;
      }
      break;
    case WS_HUNT:
      if (b == CTRL_ETX)
        this->ws_ = WS_BOUNDARY;
      break;
  }
}

void IRAM_ATTR NibeBus::gap_(uint32_t ts) {
  this->push_wire_gap_(ts);
  this->ws_ = WS_BOUNDARY;
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
  if (!this->emulating() || !this->screen_.any())
    return;
  uint32_t h = q_load_(&this->txq_head_);
  uint32_t t = q_load_(&this->txq_tail_);
  uint32_t t0 = t;
  while (h != t && this->tx_queue_[t % TX_QUEUE_N].repeatable)
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
      this->reply_byte_ = ctrl_var;  // echo discriminator, armed at the real TX moment
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
  this->parser_.set_sink(this);
  this->parser_.set_address(this->my_addr_);
  this->scheduler_.set_fill_target(TX_QUEUE_N / 2);
  this->scheduler_.add(&this->content_query_);
  this->scheduler_.add(&this->key_query_);
  this->scheduler_.add(&this->register_query_);
  this->scheduler_.add(&this->menu_state_query_);
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

uint32_t NibeBus::live_tail_() {
  // once content is complete, requests are dead weight: look past them (the ISR purges them for real)
  uint32_t t = q_load_(&this->txq_tail_);
  if (this->screen_.any()) {
    uint32_t h = q_load_(&this->txq_head_);
    while (t != h && this->tx_queue_[t % TX_QUEUE_N].repeatable)
      t = (t + 1) % (2 * TX_QUEUE_N);
  }
  return t;
}

bool NibeBus::queue_empty() { return q_load_(&this->txq_head_) == this->live_tail_(); }

bool NibeBus::queue_has_once() {
  uint32_t h = q_load_(&this->txq_head_);
  for (uint32_t i = this->live_tail_(); i != h; i = (i + 1) % (2 * TX_QUEUE_N))
    if (!this->tx_queue_[i % TX_QUEUE_N].repeatable)
      return true;
  return false;
}

uint32_t NibeBus::queue_used() {
  uint32_t h = q_load_(&this->txq_head_);
  return (h - this->live_tail_() + 2 * TX_QUEUE_N) % (2 * TX_QUEUE_N);
}

bool NibeBus::send(uint8_t cmd, const uint8_t *data, size_t len, Repeat repeat) {
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
  slot.repeatable = (repeat == Repeat::YES);
  memcpy(slot.buf, buf, n);
  q_store_(&this->txq_head_, next);
  return true;
}

void NibeBus::refill_queries_() {
  // request pump: "40 <sys> 00 00" every poll until the FIRST content row (protocol.md "The display slot")
  if (!this->emulating())
    return;
  this->scheduler_.refill(*this);
}

bool NibeBus::queue_key(uint8_t key) {
  return this->queue_key_number(key, this->request_byte_);
}

bool NibeBus::queue_key_number(uint8_t key, uint8_t value) {
  if (!this->emulating()) {
    ESP_LOGW(TAG, "role is %s: key 0x%02X val 0x%02X not queued (refuses to transmit)", this->role_name(), key, value);
    return false;
  }
  return this->key_query_.press(*this, key, value);
}

bool NibeBus::queue_register_read() {
  if (!this->emulating()) {
    ESP_LOGW(TAG, "role is %s: register read 05 %02X not queued (refuses to transmit)",
             this->role_name(), this->register_addr_);
    return false;
  }
  ESP_LOGI(TAG, "queueing register read 0x72: 05 %02X (answer lands in the frame log)", this->register_addr_);
  return this->register_query_.read(*this, this->register_addr_);
}

bool NibeBus::queue_state_query() {
  if (!this->emulating()) {
    ESP_LOGW(TAG, "role is %s: state query 0x32 not queued (refuses to transmit)", this->role_name());
    return false;
  }
  ESP_LOGI(TAG, "queueing 0x32 state query (experiment: answer, ETX or silence?)");
  return this->menu_state_query_.ask(*this);
}

bool NibeBus::trigger_announce() {
  if (!this->emulating())
    return false;
  // fresh request cycle after re-arm: the repeat pump re-runs until the first content row
  this->screen_.clear_seen();
  this->content_query_.arm();
  this->scheduler_.reset_governor();
  this->refill_queries_();
  return true;
}

void NibeBus::tail_(uint32_t ts, char *out, size_t cap) {
  if (ts == 0) {
    out[0] = 0;  // synthetic loop-time events (no wire moment)
    return;
  }
  uint32_t d = ts - this->last_ev_ts_;  // unsigned wrap-safety across rolls
  snprintf(out, cap, " @%lu (+%lu)", (unsigned long) ts, (unsigned long) d);
  this->last_ev_ts_ = ts;
}

void NibeBus::parse_wire_() {
  if (this->wire_lost_) {  // bytes were dropped: what survived is a torn stream — resync before using it
    this->wire_lost_ = false;
    this->parser_.reset();
  }
  uint32_t h = q_load_(&this->wire_head_);
  uint32_t t = q_load_(&this->wire_tail_);
  while (t != h) {
    this->parser_.feed(this->wire_[t]);
    t = (t + 1) % WIRE_RING_N;
  }
  q_store_(&this->wire_tail_, t);
}

void NibeBus::drain_tx_notices_() {
  uint32_t h = q_load_(&this->tx_notice_head_);
  uint32_t t = q_load_(&this->tx_notice_tail_);
  while (t != h) {
    const TxNotice &n = this->tx_notices_[t % TX_NOTICE_N];
    char tail[28];
    this->tail_(n.ts, tail, sizeof(tail));
    switch (n.kind) {
      case TXN_REPLY:
        ESP_LOGI(TAG, "tx: %02X len=%u%s", n.byte, n.len, tail);
        break;
      case TXN_FRAME: {
        ESP_LOGI(TAG, "tx: %02X len=%u%s", n.byte, n.len, tail);
        // same format as received frames — our own sent frame, byte-for-byte
        uint8_t fbuf[TX_FRAME_MAX];
        uint8_t len = this->tx_log_len_ < TX_FRAME_MAX ? this->tx_log_len_ : TX_FRAME_MAX;
        for (uint8_t i = 0; i < len; i++)  // stack copy off the volatile mirror
          fbuf[i] = this->tx_log_buf_[i];
        if (len >= 5 && fbuf[0] == n.byte) {
          char hexbuf[3 * 32 + 1];
          hexbuf[0] = 0;
          if (fbuf[3] > 0 && fbuf[3] <= 32)
            hex_(hexbuf, sizeof(hexbuf), &fbuf[4], fbuf[3]);
          ESP_LOGI(TAG, "  tx frame cmd=%02X sender=%02X%02X len=%u data=%s chk=%02X", fbuf[0], fbuf[1],
                   fbuf[2], (unsigned) fbuf[3], hexbuf, fbuf[4 + fbuf[3]]);
        }
        if (n.byte == CMD_ANNOUNCE)
          this->refill_queries_();
        break;
      }
      case TXN_FEED_DONE:
        // paced feed done: report the achieved inter-byte cadence
        ESP_LOGI(TAG, "tx feed done: gaps %lu-%lu us%s", (unsigned long) this->paced_gap_min_,
                 (unsigned long) this->paced_gap_max_, tail);
        break;
      case TXN_TX_REFUSED:
        ESP_LOGW(TAG, "anomaly: tx refused (dropped, NOT retried)%s", tail);
        break;
    }
    t = (t + 1) % TX_NOTICE_N;
  }
  q_store_(&this->tx_notice_tail_, t);
}

void NibeBus::on_poll(uint32_t ts, uint16_t addr) {
  this->last_reg_query_valid_ = false;  // a new exchange opens: any pending register answer is gone
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  if (addr != this->my_addr_) {
    this->polls_other_++;
    if (this->log_level_ >= 2)
      ESP_LOGD(TAG, "poll %04X%s", addr, tail);
    return;
  }
  this->polls_own_++;
  this->last_own_poll_ms_ = esphome::millis();  // loop-clock bookkeeping
  ESP_LOGD(TAG, "poll %04X (own)%s", addr, tail);
  if (!this->emulating())
    return;
  const uint8_t reply = this->scheduler_.poll_reply(*this);
  // mute-turnaround: the reply tick fires the answer into the master's listening window (+0.8-2.4 ms; protocol.md)
  this->arm_reply_(RP_CTRL, reply, ts, POLL_REPLY_DELAY_US);
  // no self-close: the MASTER closes idle exchanges — our self-close collided with its close and corrupted the bus
}

void NibeBus::on_ctrl(uint32_t ts, uint8_t byte, uint16_t ctx) {
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  if (this->log_level_ >= 2)
    ESP_LOGD(TAG, "ctrl %02X (slot %04X)%s", byte, ctx, tail);
}

void NibeBus::on_gap(uint32_t ts, uint32_t gap_us) {
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  this->last_gap_us_ = gap_us > 0xFFFF ? 0xFFFF : gap_us;
  if (this->log_level_ >= 2)
    ESP_LOGD(TAG, "gap %lu us%s", (unsigned long) gap_us, tail);
}

void NibeBus::on_len_rescue(uint32_t ts, uint8_t declared_len) {
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  if (this->log_level_ >= 2)
    ESP_LOGD(TAG, "frame len rescue: declared %u, one extra byte absorbed%s", declared_len, tail);
}

void NibeBus::on_anomaly(uint32_t ts, uint8_t code) {
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  this->anomalies_++;
  const char *why = "?";
  switch (code) {
    case AN_DESYNC_HUNT: why = "checksum failed -> hunting ETX"; break;
    case AN_FRAME_ABORT: why = "frame len absurd"; break;
    case AN_OWN_POLL_TIMEOUT: why = "master stopped polling us"; break;
    case AN_STRAY_CTRL: why = "control byte/reply missing somewhere"; break;
  }
  ESP_LOGW(TAG, "anomaly: %s%s", why, tail);
  if ((code == AN_DESYNC_HUNT || code == AN_FRAME_ABORT || code == AN_STRAY_CTRL) && this->bus_ != nullptr)
    this->bus_->debug_dump_recent();
}

void NibeBus::on_frame_bad(uint32_t ts, uint8_t cmd, uint16_t sender, uint8_t len) {
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  ESP_LOGW(TAG, "BAD frame cmd=%02X sender=%04X len=%u%s", cmd, sender, len, tail);
}

void NibeBus::on_frame(uint32_t ts, uint8_t cmd, uint16_t sender, const uint8_t *payload, uint8_t len,
                       bool in_our_slot) {
  char tail[28];
  this->tail_(ts, tail, sizeof(tail));
  const uint8_t n = len > FRAME_MAX_DATA ? (uint8_t) FRAME_MAX_DATA : len;
  const FrameView view{cmd, sender, payload, n};
  char hexbuf[3 * FRAME_MAX_DATA + 1];
  hexbuf[0] = 0;
  hex_(hexbuf, sizeof(hexbuf), payload, n);
  if (this->log_level_ >= 1) {
    ESP_LOGD(TAG, "frame cmd=%02X sender=%04X len=%u data=%s%s", cmd, sender, (unsigned) len, hexbuf, tail);
    if (this->log_level_ >= 2)
      ESP_LOGD(TAG, "  intra-byte gap max: %lu us", (unsigned long) this->parser_.last_frame_gap_us());
  }

  // display rows ONLY in our own slot — a probe must not count the panel's rows (dead-arms the pump)
  if (sender == ADDR_VVM300 && in_our_slot && cmd >= CMD_ROW_BASE && cmd <= CMD_ROW_BASE + 3) {
    uint8_t row = cmd - CMD_ROW_BASE + 1;
    if (row >= 2) {  // rows 2..4: latin-1 text, NUL padded
      bool first_content = !this->screen_.any();
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
      this->screen_.set_row(row, text.c_str());
      if (first_content) {
        // the repeat phase ends at the FIRST content row (protocol.md "The display slot")
        this->content_query_.disarm();  // queued requests pop at the next poll
        ESP_LOGI(TAG, "first content row (row %u) received: request pump off", row);
      }
      if (this->screen_.complete()) {
        ESP_LOGI(TAG, "rows 2-4 received: display content complete");
      }
#ifdef USE_TEXT_SENSOR
      for (const auto &sink : this->row_sinks_)
        if (sink.row == row)
          sink.sensor->publish_state(this->screen_.row(row));
#endif
    } else {  // row 1: the frame carries more than the 3-byte strip — publish the raw frame for decoding
      this->screen_.set_symbols(payload, n);
#ifdef USE_TEXT_SENSOR
      for (const auto &sink : this->row_sinks_)
        if (sink.row == 1)
          sink.sensor->publish_state(hexbuf);
#endif
    }
  }

  // F2025 sensor block: passive decode only, we are not part of that exchange (commands.md)
  if (sender == ADDR_F2025 && cmd == CMD_SENSOR) {
#ifdef USE_TEXT_SENSOR
    for (const auto &sink : this->f2025_word_hex_sinks_) {
      int16_t raw = 0;
      if (f2025_word(view, sink.first, &raw)) {
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%04X", (unsigned) (uint16_t) raw);
        sink.second->publish_state(hex);
      }
    }
#endif
    if (this->log_level_ >= 1) {
      float celsius[5] = {};
      for (uint8_t i = 0; i < 5; i++) {
        int16_t raw = 0;
        if (f2025_word(view, i, &raw))
          celsius[i] = celsius_from_tenths(raw);
      }
      ESP_LOGD(TAG, "F2025 sensor block (word0..4: %.1f %.1f %.1f %.1f %.1f)", celsius[0], celsius[1],
               celsius[2], celsius[3], celsius[4]);
    }
  }

#ifdef USE_SENSOR
  // generic F2025 answer register: u16/s16 LE at a byte offset, scaled (commands.md)
  if (sender == ADDR_F2025) {
    for (const auto &sink : this->f2025_reg_sinks_) {
      if (sink.first.cmd != cmd)
        continue;
      float value = 0;
      if (f2025_register(view, sink.first.offset, sink.first.is_signed, sink.first.scale, &value))
        sink.second->publish_state(value);
    }
  }
#endif

#ifdef USE_TEXT_SENSOR
  // F2025 frame hex for the decoding watch (commands.md)
  if (sender == ADDR_F2025) {
    for (const auto &sink : this->f2025_frame_hex_sinks_)
      if (sink.first == cmd)
        sink.second->publish_state(hexbuf);
  }
#endif

  // passive register pairing: the master's 0x72 request names the register; a 0x72 from another
  // node before the next poll is its answer. The value grammar is still open, so log the raw bytes.
  if (cmd == CMD_REG_READ) {
    uint8_t reg = 0;
    if (sender == ADDR_VVM300 && parse_register_request(view, &reg)) {
      this->last_reg_query_ = reg;
      this->last_reg_query_valid_ = true;
    } else if (sender != ADDR_VVM300 && this->last_reg_query_valid_) {
      ESP_LOGI(TAG, "register 0x%02X answer: %s", this->last_reg_query_, hexbuf);
      this->last_reg_query_valid_ = false;  // one answer per request
    }
  }

  // -- our address emitting = panel traffic (physical panel, or our own frames echoed) --
  // key frames: data[0] discriminates; data[1] is the relayed system value (commands.md)
  if (sender == this->my_addr_ && cmd == CMD_KEY && len == 4 && n >= 2 && payload[0] <= 4) {
    uint8_t key = payload[0];
    const char *name = KEY_NAMES[key];
    ESP_LOGI(TAG, "key frame: 0x%02X (%s)", key, name);
#ifdef USE_TEXT_SENSOR
    if (this->key_name_sink_ != nullptr)
      this->key_name_sink_->publish_state(name);
#endif
  }
  if (sender == this->my_addr_ && cmd == CMD_ANNOUNCE && len == 4 && n >= 2 && payload[0] == 0x40) {
    ESP_LOGI(TAG, "display request frame from %04X (relay byte: %02X)", sender, payload[1]);
  }

  // 0x22 is the master->F2025 boot link check; announces are self-initiated (hypotheses.md)
  if (this->emulating() && sender == ADDR_VVM300 && in_our_slot && cmd == CMD_MENU2 && this->menu_reply_) {
    ESP_LOGI(TAG, "master 0x62 menu request: queueing reply");
    uint8_t data[1] = {0x0F};
    this->send(CMD_MENU2, data, 1, Repeat::NO);
  }

  // clock query: 7-byte BCD + day-of-week reply; opt-in — it sets the master's clock (protocol.md)
  if (this->emulating() && this->d4_reply_ && sender == ADDR_VVM300 && in_our_slot && cmd == CMD_CLOCK) {
#ifdef USE_TIME
    esphome::ESPTime t = this->time_source_ != nullptr ? this->time_source_->now() : esphome::ESPTime();
#else
    // no time platform: host epoch is real time, a bare ESP starts at 1970 — is_valid() silences that
    esphome::ESPTime t = esphome::ESPTime::from_epoch_local(::time(nullptr));
#endif
    if (!t.is_valid()) {
      ESP_LOGW(TAG, "master 0xD4 clock query: our clock is not synced/valid — not answering");
    } else {
      uint8_t d4_tail = this->d4_tail_ != 0 ? this->d4_tail_ : t.day_of_week;
      uint8_t data[7];
      encode_d4_clock(t.year, (uint8_t) t.month, (uint8_t) t.day_of_month, (uint8_t) t.hour,
                      (uint8_t) t.minute, (uint8_t) t.second, d4_tail, data);
      ESP_LOGI(TAG, "master 0xD4 clock query: queueing %02X %02X %02X %02X %02X %02X %02X "
                    "(%04u-%02u-%02u %02u:%02u:%02u)",
               data[0], data[1], data[2], data[3], data[4], data[5], data[6], (unsigned) t.year, (unsigned) t.month,
               (unsigned) t.day_of_month, (unsigned) t.hour, (unsigned) t.minute, (unsigned) t.second);
      this->send(CMD_CLOCK, data, 7, Repeat::NO);
    }
  }

  // a valid frame in our slot is receipt-ACKed late, when quiet (>= PUSH_ACK_DELAY_US; protocol.md)
  if (in_our_slot && this->emulating()) {
    uint8_t wr = this->push_ack_wr_;
    if ((uint8_t) (wr - this->push_ack_rd_) < 4) {
      this->push_acks_[wr % 4] = ts;
      this->push_ack_wr_ = (uint8_t) (wr + 1);
    } else {
      this->on_anomaly(ts, AN_STRAY_CTRL);  // more pushes than the panel would queue receipts for
    }
  }
}

void NibeBus::emulator_push_ack_tick_() {
  // fire ONE aged push receipt per pass, only when the line is quiet (protocol.md)
  if (!this->emulating() || this->bus_ == nullptr)
    return;
  uint8_t rd = this->push_ack_rd_;
  uint8_t wr = this->push_ack_wr_;
  if ((uint8_t) (wr - rd) == 0)
    return;
  if (this->bus_->rx_silence_us() < 2500)  // mid-burst: never collide with the stream
    return;
  uint32_t now = (uint32_t) esphome::micros();
  uint32_t arm_ts = this->push_acks_[rd % 4];
  if ((now - arm_ts) < PUSH_ACK_DELAY_US)  // render lag not elapsed yet
    return;
  this->push_ack_rd_ = (uint8_t) (rd + 1);  // pop: refused sends still drop, never retried
  uint8_t ack = CTRL_ACK;  // stack copy (kept off flash on every path, as usual)
  if (this->bus_->send_burst(&ack, 1, false)) {
    this->tx_sent_++;
    ESP_LOGI(TAG, "tx: %02X len=0", CTRL_ACK);
  } else {
    this->tx_refused_++;
  }
}

void NibeBus::loop() {
  // wire facts -> parser -> handlers, all task-side (ADR-0001)
  this->parse_wire_();
  this->drain_tx_notices_();

  // top up the request queue from loop too: row pushes produce no tx notice to refill from
  this->refill_queries_();

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
    this->on_anomaly(0, AN_OWN_POLL_TIMEOUT);
    this->last_own_poll_ms_ = now;  // re-arm
  }

  if (this->next_summary_ms_ == 0 || now >= this->next_summary_ms_) {
    this->next_summary_ms_ = now + 10000;
    const auto bs = this->bus_ != nullptr ? this->bus_->stats() : rs485_bus::BusStats{};
    {
      // stack copies: the summary log hit an xtensa GCC ICE with 11 direct volatile args
      const unsigned f_ok = this->parser_.frames_ok(), f_bad = this->parser_.frames_bad(), p_own = this->polls_own_,
                     p_other = this->polls_other_, txs = this->tx_sent_, txr = this->tx_refused_,
                     rdrop = this->reply_drops_, ech = this->echo_filtered_, anom = this->anomalies_,
                     evov = this->wire_overflow_;
      ESP_LOGI(TAG, "bus: frames ok=%u bad=%u polls_own=%u other=%u tx=%u txref=%u replydrop=%u gaps=%u echoes=%u anomalies=%u wire_drop=%u",
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
        case 0: s->publish_state(this->parser_.frames_ok()); break;
        case 1: s->publish_state(this->parser_.frames_bad()); break;
        case 2: s->publish_state(this->polls_own_); break;
        case 3: s->publish_state(this->polls_other_); break;
        case 4: s->publish_state(this->tx_sent_); break;
        case 5: s->publish_state(this->anomalies_); break;
        case 6: s->publish_state(this->echo_filtered_); break;
        case 7: s->publish_state(this->wire_overflow_); break;
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
                this->screen_.seen(2) ? 1 : 0, this->screen_.seen(3) ? 1 : 0,
                this->screen_.seen(4) ? 1 : 0, this->content_query_.active() ? "ON" : "OFF",
                this->menu_reply_ ? "ON" : "OFF");
  if (this->bus_ != nullptr)
    ESP_LOGCONFIG(TAG, "  9-bit line: TX marks ENQ 1 / data 0; RX carries the slot as a per-byte fact");
  {
    char units[96] = "";
    for (size_t i = 0; i < this->scheduler_.count(); i++) {
      if (i > 0)
        strlcat(units, ", ", sizeof(units));
      strlcat(units, this->scheduler_.at(i)->name(), sizeof(units));
    }
    ESP_LOGCONFIG(TAG, "  outbound units: %s", units);
  }
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
  if (this->state_query_) {
    this->hub_->queue_state_query();
  } else if (this->read_register_) {
    this->hub_->queue_register_read();  // target pinned by the register-address number
  } else if (this->announce_) {
    this->hub_->trigger_announce();
  } else {
    this->hub_->queue_key(this->key_);
  }
}
#endif  // USE_BUTTON

void NibeBus::set_knob_placement(uint8_t position) {
  this->screen_.set_knob(position);
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
