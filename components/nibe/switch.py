import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import CONF_NIBE_ID, NibeBus, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "switch"]

NibeSwitch = nibe_ns.class_("NibeSwitch", switch.Switch, cg.Component)

# the arm switch: staged transmit gate. ON = role live + fresh announce; OFF = kill switch
# (the engine goes silent; the master is in absent-node backoff within one poll)
CONFIG_SCHEMA = switch.switch_schema(NibeSwitch, default_restore_mode="ALWAYS_OFF").extend(
    {
        cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)
    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(var.set_hub(hub))
