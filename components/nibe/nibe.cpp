// Hub core: lifecycle, config, public API and the poll/ENQ exchange
// (port of the Python NibeClient poll handling).
#include "nibe.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nibe {

static const char *const TAG = "nibe";

void NibeComponent::setup() {
  // Emulate the display's boot announcement (captured on the real panel):
  // on the first poll we answer ENQ and send cmd 0x40 '40 85 00 00'.
  this->queue_frame(NIBE_CMD_BUTTON, {NIBE_ANNOUNCE[0], NIBE_ANNOUNCE[1], NIBE_ANNOUNCE[2], NIBE_ANNOUNCE[3]});
}

void NibeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Nibe bus:");
  for (uint16_t a : this->addresses_)
    ESP_LOGCONFIG(TAG, "  answering polls for address 0x%04X", a);
  ESP_LOGCONFIG(TAG, "  passive: %s, heartbeat: %s", YESNO(this->passive_), YESNO(this->heartbeat_));
}

void NibeComponent::loop() {
  while (this->available() > 0) {
    uint8_t b;
    this->read_array(&b, 1);
    this->feed_byte(b);
  }
}

bool NibeComponent::is_ours(uint16_t address) const {
  for (uint16_t a : this->addresses_)
    if (a == address)
      return true;
  return false;
}

void NibeComponent::queue_write(uint8_t index, const std::vector<uint8_t> &raw) {
  this->write_queue_.emplace_back(index, raw);
}

void NibeComponent::queue_frame(uint8_t cmd, const std::vector<uint8_t> &data) {
  this->frame_queue_.emplace_back(cmd, data);
}

void NibeComponent::press_button(uint8_t code) {
  // Display button press frame: cmd 0x40, data [button, 0x85, 0, 0].
  this->queue_frame(NIBE_CMD_BUTTON, {code, 0x85, 0x00, 0x00});
}

void NibeComponent::set_knob(uint8_t position) {
  // Display knob frame: cmd 0x40, data [0x40, pos, 0, 0] (curve offset adjust).
  this->queue_frame(NIBE_CMD_BUTTON, {0x40, position, 0x00, 0x00});
}

void NibeComponent::send_frame(uint8_t cmd, const std::vector<uint8_t> &data) {
  if (data.size() > 255)
    return;
  uint8_t frame[4 + 255 + 1];
  frame[0] = cmd;
  frame[1] = (uint8_t) (this->our_address() >> 8);
  frame[2] = (uint8_t) this->our_address();
  frame[3] = (uint8_t) data.size();
  for (size_t i = 0; i < data.size(); i++)
    frame[4 + i] = data[i];
  size_t total = 4 + data.size();
  frame[total] = xor8_plain(frame, total);
  this->write_array(frame, total + 1);
  this->note_sent_frame();
}

// ------------------------------------------------- exchange (client port)

void NibeComponent::handle_poll(uint16_t address) {
  this->last_poll_was_ours_ = this->is_ours(address);
  if (this->passive_ || !this->last_poll_was_ours_)
    return;  // not ours; just sniffing

  // announce/queued frames take the poll first; a pending write is only
  // popped when it will actually be used (the Python original could drop one).
  bool have_announce = false;
  std::pair<uint8_t, std::vector<uint8_t>> announce{0, {}};
  if (!this->frame_queue_.empty()) {
    announce = this->frame_queue_.front();
    this->frame_queue_.pop_front();
    have_announce = true;
  }
  if (!have_announce && this->boot_announces_ > 0) {
    this->boot_announces_--;
    announce = {NIBE_CMD_BUTTON, {NIBE_ANNOUNCE[0], NIBE_ANNOUNCE[1], NIBE_ANNOUNCE[2], NIBE_ANNOUNCE[3]}};
    have_announce = true;
  }
  if (!have_announce && this->heartbeat_) {
    announce = {NIBE_CMD_BUTTON, {NIBE_ANNOUNCE[0], NIBE_ANNOUNCE[1], NIBE_ANNOUNCE[2], NIBE_ANNOUNCE[3]}};
    have_announce = true;
  }

  if (have_announce) {
    this->pending_ = announce;
    this->pending_is_write_ = false;
    this->await_master_ack_ = true;
    this->send_byte(NIBE_ENQ);  // master's ACK to our ENQ arrives at P_ENQ_ACK
    ESP_LOGD(TAG, "ENQ -> queued frame cmd=0x%02X", announce.first);
    return;
  }

  if (!this->write_queue_.empty()) {
    this->pending_ = this->write_queue_.front();
    this->pending_is_write_ = true;
    this->write_queue_.pop_front();
    this->await_master_ack_ = true;
    this->send_byte(NIBE_ENQ);
    ESP_LOGD(TAG, "ENQ -> queued write of param 0x%02X", this->pending_.first);
    return;
  }

  this->send_byte(NIBE_ACK);
  this->note_self_response();  // our ACK is not echoed; next RX is master's frame/ETX
}

void NibeComponent::handle_control(uint8_t value) {
  if (this->await_master_ack_ && value == NIBE_ACK) {
    // master accepted our ENQ: send the queued frame now
    this->await_master_ack_ = false;
    this->await_frame_ack_ = true;
    if (this->pending_is_write_) {
      std::vector<uint8_t> payload = {0x00, this->pending_.first};
      payload.insert(payload.end(), this->pending_.second.begin(), this->pending_.second.end());
      this->send_frame(NIBE_CMD_PARAMS, payload);
      ESP_LOGD(TAG, "sent param write 0x%02X (%u bytes)", this->pending_.first,
               (unsigned) this->pending_.second.size());
    } else {
      this->send_frame(this->pending_.first, this->pending_.second);
      ESP_LOGD(TAG, "sent frame cmd=0x%02X", this->pending_.first);
    }
  } else if (this->await_frame_ack_ && value == NIBE_ACK) {
    // our frame was accepted: close the exchange with ETX (sender closes)
    this->await_frame_ack_ = false;
    this->send_byte(NIBE_ETX);
  }
}

}  // namespace nibe
}  // namespace esphome
