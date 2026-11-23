import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from . import CONF_NIBE_ID, NibeBus, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "number"]

CONF_KNOB = "knob"  # true: the front-panel knob placement number

# Front-panel knob: value = placement (u8); control() queues [KEY_KNOB, placement, 0, 0] (commands.md).
# data[1] carries the placement where keys/requests relay the system byte.
#
# UNVERIFIED (hypotheses.md): the 0x03 id and the placement coding;
# the 0xC0/0x0B forum map belongs to another product line — rejected.
#
# Emulator role required; no readback exists — the published state is the optimistic echo.
NibeKnobNumber = nibe_ns.class_("NibeKnobNumber", number.Number, cg.Component)

CONFIG_SCHEMA = cv.All(
    number.number_schema(NibeKnobNumber)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
            cv.Required(CONF_KNOB): True,
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Optional(CONF_STEP, default=1): cv.positive_float,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
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
