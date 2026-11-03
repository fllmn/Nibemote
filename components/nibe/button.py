import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from . import CONF_NIBE_ID, NibeComponent, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe"]

CONF_CODE = "code"

NibeButton = nibe_ns.class_("NibeButton", button.Button, cg.Component)

CONFIG_SCHEMA = (
    button.button_schema(NibeButton)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeComponent),
            # button byte sent in the cmd 0x40 frame
            # (captured: 0x01 back, 0x02 next, 0x04 enter, 0x08/0x10/0x20 TBD)
            cv.Required(CONF_CODE): cv.hex_int_range(min=0, max=0xFF),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)

    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(var.set_hub(hub))
    cg.add(var.set_code(config[CONF_CODE]))
    cg.add(hub.register_button(var))
