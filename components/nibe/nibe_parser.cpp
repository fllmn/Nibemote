// Byte-stream parser and inbound frame handling (port of the Python
// LegacyParser state machine plus the 0xC0 parameter group parser).
#include "nibe.h"
#include "esphome/core/log.h"

#include <utility>

namespace esphome {
namespace nibe {

static const char *const TAG = "nibe";

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

  if (this->frame_cmd_ == NIBE_CMD_BOOT_PING && this->frame_sender_ == NIBE_ADDR_VVM_CPU &&
      this->last_poll_was_ours_) {
    // master's boot ping: announce 0x40 on the next few polls, then plain-ACK
    this->boot_announces_ = 3;
    ESP_LOGD(TAG, "cmd 0x22 boot ping -> announce 0x40 on next 3 polls");
  } else if (this->frame_cmd_ == NIBE_CMD_MENU_REQ && this->frame_sender_ == NIBE_ADDR_VVM_CPU &&
             this->last_poll_was_ours_) {
    // master asks for the menu: reply with frame 62 0f on the next poll
    this->queue_frame(NIBE_CMD_MENU_REQ, {0x0F});
    ESP_LOGD(TAG, "cmd 0x62 -> queued reply 62 0f");
  }

  if (this->frame_cmd_ == NIBE_CMD_PARAMS) {
    for (auto &kv : parse_legacy_params(std::vector<uint8_t>(data, data + data_len))) {
      ESP_LOGD(TAG, "param 0x%02X raw=%s", kv.first, format_hex(kv.second).c_str());
      this->dispatch_param(kv.first, kv.second);
    }
  } else if (this->frame_cmd_ >= NIBE_CMD_TEXT && this->frame_cmd_ <= NIBE_CMD_TEXT + 3) {
#ifdef USE_TEXT_SENSOR
    // display text row: cmd 0x50..0x53 -> rows 1..4 (null-terminated ASCII)
    uint8_t row = this->frame_cmd_ - NIBE_CMD_TEXT + 1;
    size_t len = 0;
    while (len < data_len && data[len] != 0x00)
      len++;
    this->publish_display_row(row, std::string((const char *) data, len));
#endif
  }

  // ACK the frame if it followed a poll to us
  if (!this->passive_ && this->last_poll_was_ours_)
    this->send_byte(NIBE_ACK);
}

}  // namespace nibe
}  // namespace esphome
