import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_NIBE_ID, NibeBus

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "binary_sensor"]

CONF_POLLED = "polled"

def _validate(config):
    if not config.get(CONF_POLLED, False):
        raise cv.Invalid("the only supported option is 'polled: true'")
    return config

CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
            # on while the master polls our display address (off after ~5 s silence)
            cv.Optional(CONF_POLLED, default=False): cv.boolean,
        }
    ),
    _validate,
)

async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)

    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(hub.attach_polled(var))
