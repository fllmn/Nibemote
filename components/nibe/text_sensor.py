import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import CONF_NIBE_ID, NibeComponent, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe"]

CONF_PARAMETER = "parameter"
CONF_DISPLAY_ROW = "display_row"

NibeTextSensor = nibe_ns.class_("NibeTextSensor", text_sensor.TextSensor, cg.Component)


def _validate(config):
    if CONF_PARAMETER not in config and CONF_DISPLAY_ROW not in config:
        raise cv.Invalid("must specify either parameter or display_row")
    return config


CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(NibeTextSensor)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeComponent),
            # 0xC0 parameter index with enum mappings (e.g. compressor_on)
            cv.Optional(CONF_PARAMETER): cv.hex_int_range(min=0, max=0xFF),
            # display text row fed by master frames with cmd 0x50..0x53
            cv.Optional(CONF_DISPLAY_ROW): cv.int_range(min=1, max=4),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await text_sensor.register_text_sensor(var, config)
    hub = await cg.get_variable(config[CONF_NIBE_ID])
    if CONF_PARAMETER in config:
        cg.add(var.set_index(config[CONF_PARAMETER]))
        cg.add(hub.register_param_entity(var))
    else:
        cg.add(var.set_display_row(config[CONF_DISPLAY_ROW]))
        cg.add(hub.register_display_row(config[CONF_DISPLAY_ROW], var))
