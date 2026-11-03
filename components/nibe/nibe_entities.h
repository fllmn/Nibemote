// ESPHome entity classes bound to Nibe bus data.
//
// NibeParamEntity is the common interface for anything bound to a 0xC0
// parameter index; NibeButton/NibeTextSensor(display_row) are standalone.
// The hub (nibe.h) dispatches decoded bus data to these.
#pragma once

#include "esphome/core/component.h"
#include "nibe_protocol.h"

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

namespace esphome {
namespace nibe {

class NibeComponent;

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

#ifdef USE_TEXT_SENSOR
class NibeTextSensor : public text_sensor::TextSensor, public Component, public NibeParamEntity {
 public:
  void setup() override { this->publish_state(""); }
  void publish_param(const std::vector<uint8_t> &raw) override;
  // display row 1..4 (cmd 0x50..0x53); 0 when bound to a parameter instead
  void set_display_row(uint8_t row) { this->display_row_ = row; }
  uint8_t get_display_row() const { return this->display_row_; }

 protected:
  uint8_t display_row_{0};
};
#endif  // USE_TEXT_SENSOR

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

}  // namespace nibe
}  // namespace esphome
