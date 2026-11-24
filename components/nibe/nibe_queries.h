#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/hal.h"
#include "nibe_frame.h"

namespace esphome::nibe {

// Announce policy (hypotheses.md 1): 2 ENQ polls then 2 bare ACKs — emulator policy, not a wire fact.
static constexpr uint8_t ANNOUNCE_BURST = 2;
static constexpr uint8_t ANNOUNCE_BREATH = 2;

/// A frame's repeat rule, as the query that enqueues it declares it.
enum class Repeat : uint8_t { NO, YES };

/// What a query may know about the engine: send a frame, read the state it decides on.
/// One context, not three — NibeBus is the only implementer and passes itself once (ADR-0003).
class QueryContext {
 public:
  virtual ~QueryContext() = default;
  virtual bool send(uint8_t cmd, const uint8_t *data, size_t len, Repeat repeat) = 0;
  /// the master delivered the first content row, so the request phase is over
  virtual bool content_complete() = 0;
  virtual bool queue_empty() = 0;
  virtual bool queue_has_once() = 0;  // a once-off frame (key, menu reply, clock) is waiting
  virtual uint32_t queue_used() = 0;
  /// the relayed system value in cmd-0x40 frames (commands.md)
  virtual uint8_t request_byte() const = 0;
};

/// One outbound command: the frame it builds, when it repeats, what it means.
/// Task context only; adding a command means adding one unit and one registration line.
class Query {
 public:
  virtual ~Query() = default;
  virtual const char *name() const = 0;
  /// Offer one frame; false when this query has nothing to say right now.
  virtual bool pump(QueryContext &ctx) { return false; }
};

/// Content request `40 <relay> 00 00`, repeated until the master delivers the first content row.
class ContentRequestQuery : public Query {
 public:
  const char *name() const override { return "content-request"; }
  void arm() { this->active_ = true; }
  void disarm() { this->active_ = false; }
  bool active() const { return this->active_; }
  bool pump(QueryContext &ctx) override;

 private:
  bool active_{false};
};

/// Key and knob events: cmd 0x40, data `[id, value, 00, 00]` — one shot per press.
class KeyQuery : public Query {
 public:
  const char *name() const override { return "key"; }
  bool press(QueryContext &ctx, uint8_t key, uint8_t value);
};

/// Register read: cmd 0x72, data `05 <reg>` — one shot.
class RegisterQuery : public Query {
 public:
  const char *name() const override { return "register-read"; }
  bool read(QueryContext &ctx, uint8_t reg);
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
  void refill(QueryContext &ctx);
  /// The reply byte for a poll to our slot, after topping up.
  uint8_t poll_reply(QueryContext &ctx);
  void reset_governor() {
    this->enq_streak_ = 0;
    this->bare_owed_ = 0;
  }
  /// The registered command surface, for dump_config.
  size_t count() const { return this->count_; }
  Query *at(size_t i) const { return i < this->count_ ? this->queries_[i] : nullptr; }

 private:
  static constexpr size_t MAX_QUERIES = 8;
  Query *queries_[MAX_QUERIES]{};
  size_t count_{0};
  uint32_t fill_target_{1};
  uint8_t enq_streak_{0};  // consecutive ENQ-answered own polls
  uint8_t bare_owed_{0};   // bare-ACK polls remaining in the current breath
};

}  // namespace esphome::nibe
