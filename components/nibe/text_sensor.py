import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_NIBE_ID, NibeBus

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "text_sensor"]

CONF_DISPLAY_ROW = "display_row"
CONF_LAST_KEY = "last_key"
CONF_F2025_WORD_HEX = "f2025_word_hex"
CONF_F2025_FRAME_HEX = "f2025_frame_hex"

def _validate(config):
    picked = [k for k in (CONF_DISPLAY_ROW, CONF_LAST_KEY,
                         CONF_F2025_WORD_HEX, CONF_F2025_FRAME_HEX) if k in config]
    if len(picked) != 1:
        raise cv.Invalid(f"specify exactly one of display_row, last_key, "
                         f"f2025_word_hex, f2025_frame_hex (got {picked})")
    return config

CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
            # 1 = symbol strip as hex (bits unmapped); 2..4 = latin-1 text rows (commands.md)
            cv.Optional(CONF_DISPLAY_ROW): cv.int_range(min=1, max=4),
            # PLUS / MINUS / ENTER key names (commands.md)
            cv.Optional(CONF_LAST_KEY): cv.boolean,
            # raw 16-bit word of the 0x65 sensor block as "0x%04X" — the non-thermal words
            cv.Optional(CONF_F2025_WORD_HEX): cv.int_range(min=0, max=8),
            # the F2025's answer to this command as hex — e.g. 0x71, the 16-byte status block
            cv.Optional(CONF_F2025_FRAME_HEX): cv.int_range(min=0, max=0xFF),
        }
    ),
    _validate,
)

async def to_code(config):
    var = await text_sensor.new_text_sensor(config)

    hub = await cg.get_variable(config[CONF_NIBE_ID])
    if CONF_DISPLAY_ROW in config:
        cg.add(hub.attach_row_sensor(config[CONF_DISPLAY_ROW], var))
    elif CONF_F2025_WORD_HEX in config:
        cg.add(hub.attach_f2025_word_hex(config[CONF_F2025_WORD_HEX], var))
    elif CONF_F2025_FRAME_HEX in config:
        cg.add(hub.attach_f2025_frame_hex(config[CONF_F2025_FRAME_HEX], var))
    elif CONF_LAST_KEY in config:
        cg.add(hub.attach_key_name(var))
