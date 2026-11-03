// ESPHome component for the Nibe legacy RS485 bus (VVM300 + F2025).
//
// C++ port of src/nibemote/protocol.py (LegacyParser) and src/nibemote/client.py
// (NibeClient) — the bus protocol was reverse engineered in Elektronikforumet
// thread t=13714 and confirmed live on a VVM300 + F2025. The VVM300 CPU
// (address 0x000A) is the bus master and polls nodes; we impersonate the
// display card (default address 0x00F9) so the heat pump keeps talking after
// the physical display is removed.
//
// Wire format (19200 baud 8N1, half duplex):
//   poll        : <addr:u16be>                          (2 bytes)
//   node answer : 06 ACK | 05 ENQ (has data) | 15 NAK   (within a few ms)
//   data frame  : <cmd> <sender:u16be> <len> <data...> <xor8 csum>
//   end exchange: 03 ETX
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

// Entity-class includes are guarded: the build only ships components that are
// actually used, so a config with (say) no select entities has no select.h.
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

#include <vector>
#include <deque>

#include "param_table.h"

namespace esphome {
namespace nibe {

class NibeComponent;

// wire constants (see protocol.py)
static const uint8_t NIBE_ACK = 0x06;
static const uint8_t NIBE_NAK = 0x15;
static const uint8_t NIBE_ENQ = 0x05;
static const uint8_t NIBE_ETX = 0x03;
static const uint8_t NIBE_CMD_PARAMS = 0xC0;   // parameter dump / write
static const uint8_t NIBE_CMD_BUTTON = 0x40;   // button press / boot announce
static const uint8_t NIBE_CMD_TEXT = 0x51;     // 0x51/0x52/0x53 = display rows 1..3
static const uint16_t NIBE_ADDR_VVM_CPU = 0x000A;
static const uint16_t NIBE_ADDR_DISPLAY = 0x00F9;

// ---------------------------------------------------------------- entities

// Interface for anything bound to a 0xC0 parameter index.
class NibeParamEntity {
 public:
  void set_index(uint8_t index) { this->index_ = index; }
  uint8_t get_index() const { return this->index_; }
  virtual void publish_param(const std::vector<uint8_t> &raw) = 0;
  virtual ~NibeParamEntity() = default;

 protected:
  uint8_t index_{};
};

#ifdef USE_SENSOR
class NibeParamSensor : public sensor::Sensor, public Component, public NibeParamEntity {
 public:
  void setup() override { this->publish_state(NAN); }
  void publish_param(const std::vector<uint8_t> &raw) override;
};
#endif  // USE_SENSOR

class NibeTextSensor : public text_sensor::TextSensor, public Component, public NibeParamEntity {
 public:
  void setup() override { this->publish_state(""); }
  void publish_param(const std::vector<uint8_t> &raw) override;
  void set_display_row(uint8_t row) { this->display_row_ = row; }
  uint8_t get_display_row() const { return this->display_row_; }

 protected:
  uint8_t display_row_{0};
};

#ifdef USE_NUMBER
class NibeParamNumber : public number::Number, public Component, public NibeParamEntity {
 public:
  void setup() override {
    // unknown until the bus pushes a value
    this->publish_state(NAN);
  }
  void publish_param(const std::vector<uint8_t> &raw) override;
  void control(float value) override;

  // knob mode: control() drives the display knob (cmd 0x40) instead of a param write
  void set_knob_mode(bool knob) { this->knob_ = knob; }

  void set_hub(NibeComponent *hub) { this->hub = hub; }
  NibeComponent *hub{};

 protected:
  bool knob_{false};
};
#endif  // USE_NUMBER

#ifdef USE_SELECT
class NibeParamSelect : public select::Select, public Component, public NibeParamEntity {
 public:
  void setup() override { this->publish_state(""); }
  void publish_param(const std::vector<uint8_t> &raw) override;
  void control(const std::string &value) override;

  void set_hub(NibeComponent *hub) { this->hub = hub; }
  NibeComponent *hub{};
};
#endif  // USE_SELECT

#ifdef USE_BUTTON
class NibeButton : public button::Button, public Component {
 public:
  void press_action() override;

  // button byte sent in the cmd 0x40 frame
  // (captured: 0x01 back, 0x02 next, 0x04 enter, 0x08/0x10/0x20 TBD)
  void set_code(uint8_t code) { this->code_ = code; }
  void set_hub(NibeComponent *hub) { this->hub = hub; }

 protected:
  uint8_t code_{0};
  NibeComponent *hub{};
};
#endif  // USE_BUTTON

// ---------------------------------------------------------------- hub

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
  void register_display_row(uint8_t row, NibeTextSensor *ts) {
    if (row >= 1 && row <= 3)
      this->display_rows_[row - 1] = ts;
  }
  void register_button(NibeButton *btn) { this->buttons_.push_back(btn); }

  // dispatch a decoded 0xC0 parameter update to the bound entities
  void dispatch_param(uint8_t index, const std::vector<uint8_t> &raw) {
    for (auto *entity : this->param_entities_) {
      if (entity->get_index() == index)
        entity->publish_param(raw);
    }
  }

  // API used by entities / automations
  void queue_write(uint8_t index, const std::vector<uint8_t> &raw);
  void press_button(uint8_t code);       // display button press frame (cmd 0x40)
  void set_knob(uint8_t position);       // display knob frame (cmd 0x40, data 40 <pos> 00 00)
  void queue_frame(uint8_t cmd, const std::vector<uint8_t> &data);

 protected:
  // ---- parser (port of LegacyParser) ----
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
  void feed_byte(uint8_t b);

  // ---- exchange state machine (port of NibeClient) ----
  void handle_poll(uint16_t address);
  void handle_frame();
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

  // parsed frame being assembled / handled (valid inside handle_frame)
  uint8_t frame_cmd_{0};
  uint16_t frame_sender_{0};
  bool frame_checksum_ok_{false};

  // exchange bookkeeping (port of NibeClient queues/flags)
  std::deque<std::pair<uint8_t, std::vector<uint8_t>>> write_queue_{};   // param index + raw value
  std::deque<std::pair<uint8_t, std::vector<uint8_t>>> frame_queue_{};   // arbitrary cmd + data
  std::pair<uint8_t, std::vector<uint8_t>> pending_{0, {}};
  bool pending_is_write_{false};
  bool await_master_ack_{false};
  bool await_frame_ack_{false};
  int boot_announces_{0};
  uint32_t last_activity_ms_{0};

  // entities
  std::vector<NibeParamEntity *> param_entities_{};
  NibeTextSensor *display_rows_[3]{};
  std::vector<NibeButton *> buttons_{};
};

}  // namespace nibe
}  // namespace esphome
