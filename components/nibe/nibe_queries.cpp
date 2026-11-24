#include "nibe_queries.h"

namespace esphome::nibe {

bool ContentRequestQuery::pump(TxSink &tx, QueryState &state) {
  if (!this->active_ || state.content_complete())
    return false;
  uint8_t data[4] = {0x40, this->request_byte_, 0x00, 0x00};  // data[0] = content-request id (commands.md)
  return tx.send(CMD_ANNOUNCE, data, sizeof(data), true /*repeatable*/);
}

bool KeyQuery::press(TxSink &tx, uint8_t key, uint8_t value) {
  uint8_t data[4] = {key, value, 0, 0};  // data[1]: relaid system value / knob placement (commands.md)
  return tx.send(CMD_KEY, data, sizeof(data), false);
}

bool RegisterQuery::read(TxSink &tx, uint8_t reg) {
  uint8_t data[2] = {REG_PREFIX, reg};
  return tx.send(CMD_REG_READ, data, sizeof(data), false);
}

void QueryScheduler::refill(TxSink &tx, QueryState &state, QueueState &queue) {
  if (queue.has_real())
    return;  // a real answer goes first — requests pause while it is queued
  for (;;) {
    if (queue.used() >= this->fill_target_)
      return;  // fill to half: slots stay free for keys and replies
    bool queued = false;
    for (size_t i = 0; i < this->count_; i++)
      if (this->queries_[i]->pump(tx, state))
        queued = true;
    if (!queued)
      return;
  }
}

uint8_t QueryScheduler::poll_reply(TxSink &tx, QueryState &state, QueueState &queue) {
  this->refill(tx, state, queue);
  if (queue.empty()) {
    this->enq_streak_ = 0;
    this->bare_owed_ = 0;
    return CTRL_ACK;  // nothing deliverable
  }
  if (queue.has_real()) {
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
