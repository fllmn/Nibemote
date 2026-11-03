#include "nibe.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nibe {

static const char *const TAG = "nibe";

// XOR of all bytes (plain — no 0x5C escape quirk in the legacy protocol).
static uint8_t xor8_plain(const uint8_t *data, size_t len) {
  uint8_t chk = 0;
  for (size_t i = 0; i < len; i++)
    chk ^= data[i];
  return chk;
}

// Parse a 0xC0 parameter payload: repeated `00 <index> <value:1-2B>` groups.
// Value width is ambiguous on the wire. The original Python heuristic (a
// value is 2 bytes unless the next byte is a 0x00 group marker) breaks when a
// 2-byte value has a zero high byte (any factor-10 temp below 25.6, e.g.
// 21.7°C = 0x00D9) — so consult the parameter table for the value size and
// only fall back to the byte heuristic for unknown indices.
using ParsedParams = std::vector<std::pair<uint8_t, std::vector<uint8_t>>>;

static ParsedParams parse_legacy_params(const std::vector<uint8_t> &data) {
  ParsedParams out;
  size_t i = 0;
  size_t n = data.size();
  while (i < n) {
    if (data[i] != 0x00) {
      i++;
      continue;
    }
    if (i + 1 >= n)
      break;
    uint8_t index = data[i + 1];
    size_t j = i + 2;
    const ParamInfo *p = find_param(index);
    if (p != nullptr && j + p->size <= n) {
      j += p->size;  // trust the table
    } else {
      // unknown index: original heuristic
      if (j < n && data[j] != 0x00) {
        j++;
        if (j < n && data[j] != 0x00)
          j++;
      }
    }
    out.emplace_back(index, std::vector<uint8_t>(data.begin() + i + 2, data.begin() + j));
    i = j;
  }
  return out;
}

// ---------------------------------------------------------------- decode

// Port of Param.decode (params.py): big endian, signed only for size 2.
static float decode_param_numeric(const ParamInfo *p, const std::vector<uint8_t> &raw) {
  if (raw.empty())
    return NAN;
  int32_t v;
  if (p->size == 2) {
    int16_t w = ((int16_t) raw[0]) << 8;  // big endian, zero-padded when short
    if (raw.size() > 1)
      w |= (int16_t) raw[1];
    v = w;
  } else {
    v = raw[0];  // unsigned
  }
  if (p->factor != 1.0f) {
    float out = v / p->factor;
    // round(v/factor, 3)
    float scaled = out * 1000.0f;
    return (scaled >= 0 ? (float) ((int32_t) (scaled + 0.5f)) : (float) ((int32_t) (scaled - 0.5f))) / 1000.0f;
  }
  return (float) v;
}

static const char *decode_param_mapping(const ParamInfo *p, const std::vector<uint8_t> &raw) {
  if (raw.empty() || p->mappings == nullptr)
    return "";
  for (size_t i = 0; i < p->mappings_count; i++) {
    if (p->mappings[i].raw == raw[0])
      return p->mappings[i].label;
  }
  return "";
}

// Port of Param.encode: raw = round(value * factor), big endian, 2-byte signed.
static std::vector<uint8_t> encode_param_numeric(const ParamInfo *p, float value) {
  int32_t raw_v = (int32_t) (value * p->factor + (value >= 0 ? 0.5f : -0.5f));
  std::vector<uint8_t> out;
  if (p->size == 2) {
    out.push_back((uint8_t) ((raw_v >> 8) & 0xFF));
    out.push_back((uint8_t) (raw_v & 0xFF));
  } else {
    out.push_back((uint8_t) (raw_v & 0xFF));
  }
  return out;
}

// ---------------------------------------------------------------- entities

#ifdef USE_SENSOR
void NibeParamSensor::publish_param(const std::vector<uint8_t> &raw) {
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr || p->mappings != nullptr)
    return;
  this->publish_state(decode_param_numeric(p, raw));
}
#endif  // USE_SENSOR

void NibeTextSensor::publish_param(const std::vector<uint8_t> &raw) {
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr)
    return;
  this->publish_state(decode_param_mapping(p, raw));
}

#ifdef USE_NUMBER
void NibeParamNumber::publish_param(const std::vector<uint8_t> &raw) {
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr || p->mappings != nullptr)
    return;
  this->publish_state(decode_param_numeric(p, raw));
}

void NibeParamNumber::control(float value) {
  if (this->knob_) {
    this->hub->set_knob((uint8_t) (value >= 0 ? value + 0.5f : value - 0.5f));
    this->publish_state(value);
    return;
  }
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr)
    return;
  this->hub->queue_write(this->index_, encode_param_numeric(p, value));
  this->publish_state(value);  // optimistic; corrected by the next 0xC0 frame
}
#endif  // USE_NUMBER

#ifdef USE_SELECT
void NibeParamSelect::publish_param(const std::vector<uint8_t> &raw) {
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr)
    return;
  this->publish_state(decode_param_mapping(p, raw));
}

void NibeParamSelect::control(const std::string &value) {
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr || p->mappings == nullptr)
    return;
  for (size_t i = 0; i < p->mappings_count; i++) {
    if (value.equals(p->mappings[i].label)) {
      this->hub->queue_write(this->index_, {p->mappings[i].raw});
      this->publish_state(value);
      return;
    }
  }
  ESP_LOGW(TAG, "select value '%s' is not a valid mapping for param 0x%02X", value.c_str(), this->index_);
}
#endif  // USE_SELECT

#ifdef USE_BUTTON
void NibeButton::press_action() { this->hub->press_button(this->code_); }
#endif  // USE_BUTTON

// ---------------------------------------------------------------- hub

void NibeComponent::setup() {
  // Emulate the display's boot announcement (captured on the real panel):
  // on the first poll we answer ENQ and send cmd 0x40 '40 85 00 00'.
  this->queue_frame(NIBE_CMD_BUTTON, {0x40, 0x85, 0x00, 0x00});
}

void NibeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Nibe bus:");
  for (uint16_t a : this->addresses_)
    ESP_LOGCONFIG(TAG, "  answering polls for address 0x%04X", a);
  ESP_LOGCONFIG(TAG, "  passive: %s, heartbeat: %s", YESNO(this->passive_), YESNO(this->heartbeat_));
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

// ------------------------------------------------------------ parser port

void NibeComponent::feed_byte(uint8_t b) {
  switch (this->parser_state_) {
    case P_ADDR:
      this->buf_[this->buf_len_++] = b;
      if (this->buf_len_ == 2) {
        this->poll_addr_ = ((uint16_t) this->buf_[0] << 8) | this->buf_[1];
        this->buf_len_ = 0;
        this->parser_state_ = P_RESP1;
        this->handle_poll(this->poll_addr_);
      }
      break;

    case P_RESP1:
      if (b == NIBE_ACK || b == NIBE_ENQ || b == NIBE_NAK) {
        this->parser_state_ = (b == NIBE_ACK) ? P_FRAME_OR_ETX : P_ENQ_ACK;
        this->handle_control(b);
        break;
      }
      if (b == 0x00 || b == 0x41) {  // new poll started without a response -> resync
        this->parser_state_ = P_ADDR;
        this->buf_len_ = 1;
        this->buf_[0] = b;
        break;
      }
      this->parser_state_ = P_ADDR;
      this->buf_len_ = 0;
      break;

    case P_ENQ_ACK:
      // master acknowledges our ENQ; next comes our frame slot
      this->parser_state_ = (b == NIBE_ACK) ? P_FRAME_OR_ETX : P_ADDR;
      if (b == NIBE_ACK || b == NIBE_NAK)
        this->handle_control(b);
      break;

    case P_FRAME_OR_ETX:
      if (b == NIBE_ETX) {
        this->parser_state_ = P_ADDR;
        this->buf_len_ = 0;
        break;
      }
      if (b == 0x00 || b == 0x41) {  // looks like a new poll's high byte -> resync
        this->parser_state_ = P_ADDR;
        this->buf_len_ = 1;
        this->buf_[0] = b;
        break;
      }
      this->buf_[0] = b;
      this->buf_len_ = 1;
      this->parser_state_ = P_FRAME_HDR;
      break;

    case P_FRAME_HDR:
      this->buf_[this->buf_len_++] = b;
      if (this->buf_len_ == 4)  // cmd + sender(2) + len
        this->parser_state_ = P_FRAME_BODY;
      break;

    case P_FRAME_BODY: {
      this->buf_[this->buf_len_++] = b;
      size_t length = this->buf_[3];
      if (this->buf_len_ == 4 + length + 1) {
        // frame complete
        this->frame_cmd_ = this->buf_[0];
        this->frame_sender_ = ((uint16_t) this->buf_[1] << 8) | this->buf_[2];
        this->frame_checksum_ok_ = this->buf_[this->buf_len_ - 1] == xor8_plain(this->buf_, this->buf_len_ - 1);
        this->buf_len_ = 0;
        this->parser_state_ = P_RESP2;
        this->handle_frame();
      }
      break;
    }

    case P_RESP2:
      this->parser_state_ = P_ETX;
      if (b == NIBE_ACK || b == NIBE_NAK || b == NIBE_ENQ) {
        this->handle_control(b);
        break;
      }
      this->parser_state_ = P_ADDR;
      break;

    case P_RESP2_SENT:
      // we transmitted a frame; the next bus byte is the ACK/NAK to it
      this->parser_state_ = P_ADDR;
      this->buf_len_ = 0;
      if (b == NIBE_ACK || b == NIBE_NAK || b == NIBE_ENQ)
        this->handle_control(b);
      break;

    case P_ETX:
      this->parser_state_ = P_ADDR;
      this->buf_len_ = 0;
      break;
  }
}

// ------------------------------------------------- exchange port (client)

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
    announce = {NIBE_CMD_BUTTON, {0x40, 0x85, 0x00, 0x00}};
    have_announce = true;
  }
  if (!have_announce && this->heartbeat_) {
    announce = {NIBE_CMD_BUTTON, {0x40, 0x85, 0x00, 0x00}};
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

namespace {
void param_dispatch(NibeComponent *hub, uint8_t index, const std::vector<uint8_t> &raw) {
  ESP_LOGD(TAG, "param 0x%02X raw=%s", index, format_hex(raw).c_str());
  hub->dispatch_param(index, raw);
}
}  // namespace

void NibeComponent::handle_frame() {
  if (!this->frame_checksum_ok_) {
    ESP_LOGW(TAG, "bad checksum frame cmd=0x%02X sender=0x%04X", this->frame_cmd_, this->frame_sender_);
    if (!this->passive_ && this->last_poll_was_ours_)
      this->send_byte(NIBE_NAK);
    return;
  }

  const uint8_t *data = this->buf_ + 4;
  size_t data_len = this->buf_[3];
  ESP_LOGV(TAG, "frame cmd=0x%02X from=0x%04X len=%u", this->frame_cmd_, this->frame_sender_,
           (unsigned) data_len);

  if (this->frame_cmd_ == 0x22 && this->frame_sender_ == NIBE_ADDR_VVM_CPU && this->last_poll_was_ours_) {
    // master's boot ping: announce 0x40 on the next few polls, then plain-ACK
    this->boot_announces_ = 3;
    ESP_LOGD(TAG, "cmd 0x22 boot ping -> announce 0x40 on next 3 polls");
  } else if (this->frame_cmd_ == 0x62 && this->frame_sender_ == NIBE_ADDR_VVM_CPU &&
             this->last_poll_was_ours_) {
    // master asks for the menu: reply with frame 62 0f on the next poll
    this->queue_frame(0x62, {0x0F});
    ESP_LOGD(TAG, "cmd 0x62 -> queued reply 62 0f");
  }

  if (this->frame_cmd_ == NIBE_CMD_PARAMS) {
    for (auto &kv : parse_legacy_params(std::vector<uint8_t>(data, data + data_len)))
      param_dispatch(this, kv.first, kv.second);
  } else if (this->frame_cmd_ >= NIBE_CMD_TEXT && this->frame_cmd_ <= NIBE_CMD_TEXT + 2) {
    // display text row (null-terminated string in the payload)
    uint8_t row = this->frame_cmd_ - NIBE_CMD_TEXT;  // 0..2
    if (row < 3 && this->display_rows_[row] != nullptr) {
      size_t len = 0;
      while (len < data_len && data[len] != 0x00)
        len++;
      this->display_rows_[row]->publish_state(std::string((const char *) data, len));
    }
  }

  // ACK the frame if it followed a poll to us
  if (!this->passive_ && this->last_poll_was_ours_)
    this->send_byte(NIBE_ACK);
}

void NibeComponent::loop() {
  while (this->available() > 0) {
    uint8_t b;
    this->read_array(&b, 1);
    this->feed_byte(b);
  }
}

}  // namespace nibe
}  // namespace esphome
