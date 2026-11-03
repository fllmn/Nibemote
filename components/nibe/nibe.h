// ESPHome hub for the Nibe legacy RS485 bus (VVM300 + F2025).
//
// Ports the exchange state machine of the original Python client (answer
// polls for our address, ENQ when we have data, close with ETX) on top of the
// byte-stream parser (nibe_parser.cpp). Entity dispatch goes to the classes
// in nibe_entities.h.
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "nibe_entities.h"
#include "nibe_protocol.h"
#include "param_table.h"

namespace esphome {
namespace nibe {

class NibeComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_addresses(std::vector<uint16_t> addresses) { this->addresses_ = std::move(addresses); }
  void set_passive(bool passive) { this->passive_ = passive; }
  void set_heartbeat(bool heartbeat) { this->heartbeat_ = heartbeat; }

  // entity registration (called from the platform codegen)
  void register_param_entity(NibeParamEntity *entity) { this->param_entities_.push_back(entity); }
#ifdef USE_TEXT_SENSOR
  void register_display_row(uint8_t row, NibeTextSensor *ts) {
    if (row >= 1 && row <= 4)
      this->display_rows_[row - 1] = ts;
  }
#endif
  void register_button(NibeButton *btn) { this->buttons_.push_back(btn); }

  // dispatch a decoded 0xC0 parameter update to the bound entities
  void dispatch_param(uint8_t index, const std::vector<uint8_t> &raw) {
    for (auto *entity : this->param_entities_) {
      if (entity->get_index() == index)
        entity->publish_param(raw);
    }
  }

  // publish a display text row (1..4)
#ifdef USE_TEXT_SENSOR
  void publish_display_row(uint8_t row, const std::string &text) {
    if (row >= 1 && row <= 4 && this->display_rows_[row - 1] != nullptr)
      this->display_rows_[row - 1]->publish_state(text);
  }
#endif

  // API used by entities / automations
  void queue_write(uint8_t index, const std::vector<uint8_t> &raw);
  void press_button(uint8_t code);   // display button press frame (cmd 0x40)
  void set_knob(uint8_t position);   // display knob frame (cmd 0x40, data 40 <pos> 00 00)
  void queue_frame(uint8_t cmd, const std::vector<uint8_t> &data);

 protected:
  // ---- parser (port of the Python LegacyParser) ----
  enum ParserState {
    P_ADDR,         // reading 2-byte poll address
    P_RESP1,        // waiting for node response byte (ours is not echoed)
    P_FRAME_OR_ETX, // after ACK: master frame or bare ETX
    P_FRAME_HDR,    // cmd + sender(2) + len
    P_FRAME_BODY,   // data + csum
    P_RESP2,        // response to master frame (we ACK)
    P_ETX,          // optional second ETX
    P_ENQ_ACK,      // after our ENQ: expect master ACK
    P_RESP2_SENT,   // we sent a frame; next byte is the ACK/NAK to it
  };
  void note_sent_frame() { this->parser_state_ = P_RESP2_SENT; }
  void note_self_response() {
    if (this->parser_state_ == P_RESP1 || this->parser_state_ == P_ENQ_ACK)
      this->parser_state_ = P_FRAME_OR_ETX;
  }
  void feed_byte(uint8_t b);        // nibe_parser.cpp
  void handle_frame();              // nibe_parser.cpp

  // ---- exchange state machine (port of the Python NibeClient) ----
  void handle_poll(uint16_t address);
  void handle_control(uint8_t value);
  void send_byte(uint8_t b) { this->write_array(&b, 1); }
  void send_frame(uint8_t cmd, const std::vector<uint8_t> &data);
  uint16_t our_address() const { return this->addresses_.empty() ? NIBE_ADDR_DISPLAY : this->addresses_[0]; }
  bool is_ours(uint16_t address) const;

  // config
  std::vector<uint16_t> addresses_{NIBE_ADDR_DISPLAY};
  bool passive_{false};
  bool heartbeat_{false};

  // parser state
  ParserState parser_state_{P_ADDR};
  uint8_t buf_[300]{};
  size_t buf_len_{0};
  uint16_t poll_addr_{0};
  bool last_poll_was_ours_{false};

  // parsed frame being handled (valid inside handle_frame)
  uint8_t frame_cmd_{0};
  uint16_t frame_sender_{0};
  bool frame_checksum_ok_{false};

  // exchange bookkeeping
  std::deque<std::pair<uint8_t, std::vector<uint8_t>>> write_queue_{};  // param index + raw value
  std::deque<std::pair<uint8_t, std::vector<uint8_t>>> frame_queue_{};  // arbitrary cmd + data
  std::pair<uint8_t, std::vector<uint8_t>> pending_{0, {}};
  bool pending_is_write_{false};
  bool await_master_ack_{false};
  bool await_frame_ack_{false};
  int boot_announces_{0};

  // entities
  std::vector<NibeParamEntity *> param_entities_{};
#ifdef USE_TEXT_SENSOR
  NibeTextSensor *display_rows_[4]{};
#endif
  std::vector<NibeButton *> buttons_{};
};

}  // namespace nibe
}  // namespace esphome
