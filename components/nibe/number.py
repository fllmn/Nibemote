import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from . import CONF_NIBE_ID, NibeComponent, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe"]

CONF_PARAMETER = "parameter"
CONF_KNOB = "knob"  # true: control the display knob (cmd 0x40), not a param write

NibeParamNumber = nibe_ns.class_("NibeParamNumber", number.Number, cg.Component)


def _validate(config):
    if CONF_PARAMETER not in config and not config.get(CONF_KNOB, False):
        raise cv.Invalid("must specify either parameter or knob")
    return config


CONFIG_SCHEMA = cv.All(
    number.number_schema(NibeParamNumber)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeComponent),
            cv.Optional(CONF_PARAMETER): cv.hex_int_range(min=0, max=0xFF),
            cv.Optional(CONF_KNOB, default=False): cv.boolean,
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Optional(CONF_STEP, default=1): cv.positive_float,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await number.register_number(
        var,
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )

    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(var.set_hub(hub))
    if CONF_PARAMETER in config:
        cg.add(var.set_index(config[CONF_PARAMETER]))
        cg.add(hub.register_param_entity(var))
    if config[CONF_KNOB]:
        cg.add(var.set_knob_mode(True))
