import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from . import CONF_NIBE_ID, NibeBus, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "number"]

CONF_KNOB = "knob"  # true: the front-panel knob placement number
CONF_REGISTER_ADDRESS = "register_address"  # true: pins the 0x72 read target for the read button

# Front-panel knob: value = placement (u8); control() queues [KEY_KNOB, placement, 0, 0] (commands.md).
# data[1] carries the placement where keys/requests relay the system byte.
#
# UNVERIFIED (hypotheses.md): the 0x03 id and the placement coding;
# the 0xC0/0x0B forum map belongs to another product line — rejected.
#
# Emulator role required; no readback exists — the published state is the optimistic echo.
NibeKnobNumber = nibe_ns.class_("NibeKnobNumber", number.Number, cg.Component)

# Register-address number: value = the register the read button probes with
# a 0x72 `05 <reg>` frame (commands.md "Register scheme"). No wire traffic by itself.
NibeRegisterNumber = nibe_ns.class_("NibeRegisterNumber", number.Number, cg.Component)


def _validate(config):
    picked = [k for k in (CONF_KNOB, CONF_REGISTER_ADDRESS) if k in config]
    if len(picked) != 1:
        raise cv.Invalid(f"specify exactly one of knob, register_address (got {picked})")
    return config


SCHEMA_COMMON = (
    {
        cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
        cv.Required(CONF_MIN_VALUE): cv.float_,
        cv.Required(CONF_MAX_VALUE): cv.float_,
        cv.Optional(CONF_STEP, default=1): cv.positive_float,
    }
)


def _schema(cls, mode):
    return cv.All(
        number.number_schema(cls)
        .extend(SCHEMA_COMMON)
        .extend(mode)
        .extend(cv.COMPONENT_SCHEMA),
    )


CONFIG_SCHEMA = cv.Any(
    _schema(NibeKnobNumber, {cv.Required(CONF_KNOB): True}),
    _schema(NibeRegisterNumber, {cv.Required(CONF_REGISTER_ADDRESS): True}),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])  # the matched cv.Any branch declared the class
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
