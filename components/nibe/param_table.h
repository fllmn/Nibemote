// Nibe legacy-bus 0xC0 parameter table.
// Faithful C++ port of src/nibemote/params.py (PARAMS) — keep both in sync!
// Protocol details: Elektronikforumet thread t=13714 (FredRovers' spec).
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace nibe {

struct ParamMapping {
  uint8_t raw;
  const char *label;
};

struct ParamInfo {
  uint8_t index;
  const char *name;
  uint8_t size;       // wire size in bytes (1 or 2)
  float factor;       // raw / factor = engineering value
  bool writable;
  const ParamMapping *mappings;  // nullptr if numeric
  size_t mappings_count;
};

// index, name, size, factor, writable, mappings
constexpr const ParamMapping kCompressorOn[] = {{0, "off"}, {2, "on"}};
constexpr const ParamMapping kOperatingMode[] = {{0, "auto"}, {1, "manual"}};

constexpr const ParamInfo PARAM_TABLE[] = {
    {0x00, "cpu_id", 1, 1, false, nullptr, 0},
    {0x01, "outdoor_temp", 2, 10, false, nullptr, 0},
    {0x02, "hot_water_sensor", 2, 10, false, nullptr, 0},
    {0x03, "exhaust_air_temp", 2, 10, false, nullptr, 0},
    {0x04, "supply_air_temp", 2, 10, false, nullptr, 0},
    {0x05, "evaporator_temp", 2, 10, false, nullptr, 0},
    {0x06, "supply_temp", 2, 10, false, nullptr, 0},
    {0x07, "return_temp", 2, 10, false, nullptr, 0},
    {0x08, "compressor_temp", 2, 10, false, nullptr, 0},
    {0x09, "immersion_heater_temp", 2, 10, false, nullptr, 0},
    {0x0A, "curve_slope", 1, 1, true, nullptr, 0},
    {0x0B, "curve_offset", 1, 1, true, nullptr, 0},
    {0x0C, "min_supply_temp", 1, 1, true, nullptr, 0},
    {0x0D, "max_supply_temp", 1, 1, true, nullptr, 0},
    {0x13, "compressor_on", 1, 1, false, kCompressorOn, 2},
    {0x15, "operating_mode", 1, 1, true, nullptr, 0},
    {0x1A, "compressor_starts", 2, 1, false, nullptr, 0},
    {0x1B, "compressor_hours", 2, 1, false, nullptr, 0},
    {0x1C, "heater_hours", 2, 1, false, nullptr, 0},
    {0x21, "operating_mode_auto", 1, 1, true, nullptr, 0},
    {0x25, "periodic_hw_interval", 2, 1, true, nullptr, 0},
    {0x26, "rcu_curve_offset", 1, 1, true, nullptr, 0},
    {0x29, "year", 1, 1, false, nullptr, 0},
    {0x2A, "month", 1, 1, false, nullptr, 0},
    {0x2B, "day", 1, 1, false, nullptr, 0},
    {0x2C, "hour", 1, 1, false, nullptr, 0},
    {0x2D, "minute", 1, 1, false, nullptr, 0},
    {0x2E, "second", 1, 1, false, nullptr, 0},
};

inline const ParamInfo *find_param(uint8_t index) {
  for (const auto &p : PARAM_TABLE) {
    if (p.index == index)
      return &p;
  }
  return nullptr;
}

}  // namespace nibe
}  // namespace esphome
