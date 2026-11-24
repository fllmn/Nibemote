#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/hal.h"
#include "nibe_frame.h"

namespace esphome::nibe {

// Announce policy (hypotheses.md 1): 2 ENQ polls then 2 bare ACKs — emulator policy, not a wire fact.
static constexpr uint8_t ANNOUNCE_BURST = 2;
static constexpr uint8_t ANNOUNCE_BREATH = 2;

/// Task-side frame producer: the engine's outbound queue.
class TxSink {
 public:
  virtual ~TxSink() = default;
  /// repeatable: a request the engine may expire (first content row) and the scheduler re-adds later.
  virtual bool send(uint8_t cmd, const uint8_t *data, size_t len, bool repeatable) = 0;
};

/// What a query may know about the panel while deciding whether to speak.
class QueryState {
 public:
  virtual ~QueryState() = default;
  /// the master delivered the first content row, so the request phase is over
  virtual bool content_complete() = 0;
};

/// What the scheduler may know about the outbound queue (the bytes stay the engine's).
class QueueState {
 public:
  virtual ~QueueState() = default;
  virtual bool empty() = 0;
  virtual bool has_real() = 0;  // a once-off frame (key, menu reply, clock) is waiting
  virtual uint32_t used() = 0;
};

/// One outbound command: the frame it builds, when it repeats, what it means.
/// Task context only (ADR-0003); adding a command means adding one unit and one registration line.
class Query {
 public:
  virtual ~Query() = default;
  virtual const char *name() const = 0;
  /// Offer one frame; false when this query has nothing to say right now.
  virtual bool pump(TxSink &tx, QueryState &state) { return false; }
};

/// Content request `40 <relay> 00 00`, repeated until the master delivers the first content row.
class ContentRequestQuery : public Query {
 public:
  const char *name() const override { return "content-request"; }
  void arm() { this->active_ = true; }
  void disarm() { this->active_ = false; }
  bool active() const { return this->active_; }
  void set_request_byte(uint8_t b) { this->request_byte_ = b; }
  bool pump(TxSink &tx, QueryState &state) override;

 private:
  uint8_t request_byte_{0x88};
  bool active_{false};
};

/// Key and knob events: cmd 0x40, data `[id, value, 00, 00]` — one shot per press.
class KeyQuery : public Query {
 public:
  const char *name() const override { return "key"; }
  bool press(TxSink &tx, uint8_t key, uint8_t value);
};

/// Register read: cmd 0x72, data `05 <reg>` — one shot.
class RegisterQuery : public Query {
 public:
  const char *name() const override { return "register-read"; }
  bool read(TxSink &tx, uint8_t reg);
};

/// Repeat scheduling and the poll-reply policy: ENQ iff something is deliverable,
/// else a burst/breath governor so the master gets bare slots to push content into.
class QueryScheduler {
 public:
  void add(Query *q) {
    if (this->count_ < MAX_QUERIES)
      this->queries_[this->count_++] = q;
  }
  /// How many entries refill tops the queue up to (the engine sets half its capacity).
  void set_fill_target(uint32_t n) { this->fill_target_ = n; }
  /// Top up repeatable frames (loop rate).
  void refill(TxSink &tx, QueryState &state, QueueState &queue);
  /// The reply byte for a poll to our slot, after topping up.
  uint8_t poll_reply(TxSink &tx, QueryState &state, QueueState &queue);
  void reset_governor() {
    this->enq_streak_ = 0;
    this->bare_owed_ = 0;
  }

 private:
  static constexpr size_t MAX_QUERIES = 8;
  Query *queries_[MAX_QUERIES]{};
  size_t count_{0};
  uint32_t fill_target_{1};
  uint8_t enq_streak_{0};  // consecutive ENQ-answered own polls
  uint8_t bare_owed_{0};   // bare-ACK polls remaining in the current breath
};

}  // namespace esphome::nibe
