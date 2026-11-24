#include "nibe_queries.h"

namespace esphome::nibe {

bool ContentRequestQuery::pump(QueryContext &ctx) {
  if (!this->active_ || ctx.content_complete())
    return false;
  uint8_t data[4] = {0x40, ctx.request_byte(), 0x00, 0x00};  // data[0] = content-request id (commands.md)
  return ctx.send(CMD_ANNOUNCE, data, sizeof(data), Repeat::YES);
}

bool KeyQuery::press(QueryContext &ctx, uint8_t key, uint8_t value) {
  uint8_t data[4] = {key, value, 0, 0};  // data[1]: relaid system value / knob placement (commands.md)
  return ctx.send(CMD_KEY, data, sizeof(data), Repeat::NO);
}

bool RegisterQuery::read(QueryContext &ctx, uint8_t reg) {
  uint8_t data[2] = {REG_PREFIX, reg};
  return ctx.send(CMD_REG_READ, data, sizeof(data), Repeat::NO);
}

void QueryScheduler::refill(QueryContext &ctx) {
  if (ctx.queue_has_once())
    return;  // a real answer goes first — requests pause while it is queued
  for (;;) {
    if (ctx.queue_used() >= this->fill_target_)
      return;  // fill to half: slots stay free for keys and replies
    bool queued = false;
    for (size_t i = 0; i < this->count_; i++)
      if (this->queries_[i]->pump(ctx))
        queued = true;
    if (!queued)
      return;
  }
}

uint8_t QueryScheduler::poll_reply(QueryContext &ctx) {
  // classify from the queue as the loop left it: refill runs after the poll (loop / tx notice),
  // exactly as the shipped governor did — refilling here would hide the empty-queue reset
  if (ctx.queue_empty()) {
    this->enq_streak_ = 0;
    this->bare_owed_ = 0;
    return CTRL_ACK;  // nothing deliverable
  }
  if (ctx.queue_has_once()) {
    this->enq_streak_ = 0;
    this->bare_owed_ = 0;
    return CTRL_ENQ;  // real answers are never throttled (don't starve)
  }
  if (this->bare_owed_ > 0) {
    this->bare_owed_--;
    return CTRL_ACK;  // breath poll: requests stay queued, the master pushes into the free slot
  }
  if (this->enq_streak_ >= ANNOUNCE_BURST) {
    this->enq_streak_ = 0;
    this->bare_owed_ = ANNOUNCE_BREATH - 1;
    return CTRL_ACK;  // burst spent: open bare slots for the master
  }
  this->enq_streak_++;
  return CTRL_ENQ;
}

}  // namespace esphome::nibe
