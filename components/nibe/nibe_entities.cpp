// Entity method implementations plus the parameter decode/encode helpers.
#include "nibe_entities.h"
#include "nibe.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nibe {

static const char *const TAG = "nibe";

// ------------------------------------------------------------ decode/encode

// Numeric decode (from the original Python Param.decode): big endian,
// signed only for size 2.
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

// Numeric encode (from the original Python Param.encode):
// raw = round(value * factor), big endian, 2-byte signed.
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

#ifdef USE_TEXT_SENSOR
void NibeTextSensor::publish_param(const std::vector<uint8_t> &raw) {
  const ParamInfo *p = find_param(this->index_);
  if (p == nullptr)
    return;
  this->publish_state(decode_param_mapping(p, raw));
}
#endif  // USE_TEXT_SENSOR

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

}  // namespace nibe
}  // namespace esphome
