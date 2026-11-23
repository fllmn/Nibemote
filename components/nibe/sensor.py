import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_NIBE_ID, NibeBus

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "sensor"]

CONF_COUNTER = "counter"
CONF_LAST_GAP_MS = "last_gap_ms"
CONF_F2025_REGISTER = "f2025_register"
CONF_CMD = "cmd"
CONF_OFFSET = "offset"
CONF_SCALE = "scale"
CONF_SIGNED = "signed"

COUNTERS = {
    "frames": 0,
    "frames_bad": 1,
    "polls_own": 2,
    "polls_other": 3,
    "tx_sent": 4,
    "anomalies": 5,
    "echo_filtered": 6,
    "ev_dropped": 7,
    "parity_errors": 8,
    "framing_errors": 9,
    "slot_ones": 10,
    "slot_zeros": 11,
}

def _validate(config):
    picked = [k for k in (CONF_COUNTER, CONF_LAST_GAP_MS, CONF_F2025_REGISTER) if k in config]
    if len(picked) != 1:
        raise cv.Invalid(f"specify exactly one of counter, last_gap_ms, "
                         f"f2025_register (got {picked})")
    return config

CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
            cv.Optional(CONF_COUNTER): cv.one_of(*COUNTERS, lower=True),
            # length of the last RX silence (calibrate gap_symbols against it)
            cv.Optional(CONF_LAST_GAP_MS): cv.boolean,
            # generic F2025 register: u16/s16 LE at a byte offset of cmd's answer, times scale (commands.md)
            cv.Optional(CONF_F2025_REGISTER): cv.Schema(
                {
                    # F2025 answer command carrying the field (0x65, 0x71, ...)
                    cv.Required(CONF_CMD): cv.int_range(min=0, max=255),
                    # byte offset of the u16 LE field inside that answer
                    cv.Required(CONF_OFFSET): cv.int_range(min=0, max=253),
                    # published = raw * scale (1.0 = raw count, 0.1 = tenths)
                    cv.Optional(CONF_SCALE, default=1.0): cv.float_,
                    # interpret as signed s16 (the 0x65 words) vs unsigned
                    cv.Optional(CONF_SIGNED, default=False): cv.boolean,
                }
            ),
        }
    ),
    _validate,
)

async def to_code(config):
    var = await sensor.new_sensor(config)

    hub = await cg.get_variable(config[CONF_NIBE_ID])
    if CONF_COUNTER in config:
        cg.add(hub.attach_counter(COUNTERS[config[CONF_COUNTER]], var))
    elif CONF_F2025_REGISTER in config:
        reg = config[CONF_F2025_REGISTER]
        cg.add(hub.attach_f2025_register(reg[CONF_CMD], reg[CONF_OFFSET],
                                         reg[CONF_SCALE], reg[CONF_SIGNED], var))
    elif CONF_LAST_GAP_MS in config:
        cg.add(hub.attach_gap_sensor(var))
