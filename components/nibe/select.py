import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID, CONF_OPTIONS

from . import CONF_NIBE_ID, NibeComponent, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe"]

CONF_PARAMETER = "parameter"

NibeParamSelect = nibe_ns.class_("NibeParamSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(NibeParamSelect)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeComponent),
            cv.Required(CONF_PARAMETER): cv.hex_int_range(min=0, max=0xFF),
            cv.Required(CONF_OPTIONS): cv.ensure_list(cv.string),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await select.register_select(var, config, options=config[CONF_OPTIONS])

    cg.add(var.set_index(config[CONF_PARAMETER]))
    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(hub.register_param_entity(var))
    cg.add(var.set_hub(hub))
