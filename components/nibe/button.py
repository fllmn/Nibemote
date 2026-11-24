import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from . import CONF_NIBE_ID, NibeBus, nibe_ns

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["nibe", "button"]

CONF_KEY = "key"
CONF_ANNOUNCE = "announce"
CONF_READ_REGISTER = "read_register"  # true: queue a 0x72 `05 <reg>` read (target: register-address number)
CONF_STATE_QUERY = "state_query"  # true: queue the 0x32 menu/state query (experiment, empty payload)

# panel key frames: cmd 0x40 data [key, <relay>, 0, 0] — ids in docs/commands.md
KEY_PLUS = 0x01
KEY_MINUS = 0x02
KEY_ENTER = 0x04
KEYS = {
    "plus": KEY_PLUS,
    "minus": KEY_MINUS,
    "enter": KEY_ENTER,
}

NibeButton = nibe_ns.class_("NibeButton", button.Button, cg.Component)


def _validate(config):
    picked = [k for k in (CONF_KEY, CONF_ANNOUNCE, CONF_READ_REGISTER, CONF_STATE_QUERY) if k in config]
    if len(picked) != 1:
        raise cv.Invalid(f"specify exactly one of key, announce, read_register, state_query (got {picked})")
    return config


CONFIG_SCHEMA = cv.All(
    button.button_schema(NibeButton)
    .extend(
        {
            cv.GenerateID(CONF_NIBE_ID): cv.use_id(NibeBus),
            cv.Optional(CONF_KEY): cv.one_of(*KEYS, lower=True),
            # re-introduce the virtual display: queue the 3 announce frames
            cv.Optional(CONF_ANNOUNCE): cv.boolean,
            # queue one register read (docs/commands.md "Register scheme"); answer in the frame log
            cv.Optional(CONF_READ_REGISTER): cv.boolean,
            # queue the 0x32 menu/state query: an experiment from this direction (empty payload)
            cv.Optional(CONF_STATE_QUERY): cv.boolean,
        }
    ),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)

    hub = await cg.get_variable(config[CONF_NIBE_ID])
    cg.add(var.set_hub(hub))
    if CONF_STATE_QUERY in config:
        cg.add(var.set_state_query())
    elif CONF_READ_REGISTER in config:
        cg.add(var.set_read_register())
    elif CONF_ANNOUNCE in config:
        cg.add(var.set_announce())
    else:
        cg.add(var.set_key(KEYS[config[CONF_KEY]]))
