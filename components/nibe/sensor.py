import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID

from . import CONF_NIBE_ID, NibeComponent, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe"]

CONF_PARAMETER = "parameter"

NibeParamSensor = nibe_ns.class_("NibeParamSensor", sensor.Sensor, cg.Component)

CONFIG_SCHEMA = (
    sensor.sensor_schema(NibeParamSensor)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeComponent),
            cv.Required(CONF_PARAMETER): cv.hex_int_range(min=0, max=0xFF),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)

    cg.add(var.set_index(config[CONF_PARAMETER]))
    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(hub.register_param_entity(var))
