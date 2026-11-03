"""Parameter index map for the legacy 0xC0 frames (forum thread t=13714).

FredRovers' index list was for a Fighter 360P; indices largely carry over to
VVM300/F2025 but verify on hardware with `nibemote params` before writing.
Only indices marked writable=True may be changed via `nibemote write`.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Param:
    index: int
    name: str
    size: int = 2  # bytes on the wire (1 or 2)
    factor: float = 10.0  # raw / factor = engineering value
    unit: str = "°C"
    writable: bool = False
    mappings: dict[int, str] = field(default_factory=dict)

    def decode(self, raw: bytes):
        if self.mappings and len(raw) >= 1:
            return self.mappings.get(raw[0], str(raw[0]))
        raw = raw[: self.size].ljust(self.size, b"\x00")
        v = int.from_bytes(raw, "big", signed=self.size == 2)
        return round(v / self.factor, 3) if self.factor != 1 else v

    def encode(self, value) -> bytes:
        if self.mappings:
            rev = {v: k for k, v in self.mappings.items()}
            raw_v = int(rev[str(value)])
        else:
            raw_v = int(round(float(value) * self.factor))
        return raw_v.to_bytes(self.size, "big", signed=self.size == 2)


# index map (360P list from the forum thread; names best-effort for VVM300/F2025)
PARAMS: dict[int, Param] = {
    0x00: Param(0x00, "cpu_id", 1, 1, "", False),
    0x01: Param(0x01, "outdoor_temp", 2, 10, "°C", False),
    0x02: Param(0x02, "hot_water_sensor", 2, 10, "°C", False),
    0x03: Param(0x03, "exhaust_air_temp", 2, 10, "°C", False),
    0x04: Param(0x04, "supply_air_temp", 2, 10, "°C", False),
    0x05: Param(0x05, "evaporator_temp", 2, 10, "°C", False),
    0x06: Param(0x06, "supply_temp", 2, 10, "°C", False),
    0x07: Param(0x07, "return_temp", 2, 10, "°C", False),
    0x08: Param(0x08, "compressor_temp", 2, 10, "°C", False),
    0x09: Param(0x09, "immersion_heater_temp", 2, 10, "°C", False),
    0x0A: Param(0x0A, "curve_slope", 1, 1, "", True),
    0x0B: Param(0x0B, "curve_offset", 1, 1, "", True),
    0x0C: Param(0x0C, "min_supply_temp", 1, 1, "°C", True),
    0x0D: Param(0x0D, "max_supply_temp", 1, 1, "°C", True),
    0x13: Param(0x13, "compressor_on", 1, 1, "", False,
                mappings={0: "off", 2: "on"}),
    0x15: Param(0x15, "operating_mode", 1, 1, "", True),
    0x1A: Param(0x1A, "compressor_starts", 2, 1, "", False),
    0x1B: Param(0x1B, "compressor_hours", 2, 1, "h", False),
    0x1C: Param(0x1C, "heater_hours", 2, 1, "h", False),
    0x21: Param(0x21, "operating_mode_auto", 1, 1, "", True),
    0x25: Param(0x25, "periodic_hw_interval", 2, 1, "days", True),
    0x26: Param(0x26, "rcu_curve_offset", 1, 1, "", True),
    0x29: Param(0x29, "year", 1, 1, "", False),
    0x2A: Param(0x2A, "month", 1, 1, "", False),
    0x2B: Param(0x2B, "day", 1, 1, "", False),
    0x2C: Param(0x2C, "hour", 1, 1, "", False),
    0x2D: Param(0x2D, "minute", 1, 1, "", False),
    0x2E: Param(0x2E, "second", 1, 1, "", False),
}


def get_param(index: int) -> Param | None:
    return PARAMS.get(index)
