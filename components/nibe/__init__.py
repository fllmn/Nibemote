import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_UART_ID

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["uart"]
MULTI_CONF = True

nibe_ns = cg.esphome_ns.namespace("nibe")
NibeComponent = nibe_ns.class_("NibeComponent", cg.Component, uart.UARTDevice)

CONF_ADDRESSES = "addresses"
CONF_HEARTBEAT = "heartbeat"
CONF_PASSIVE = "passive"
CONF_NIBE_ID = "nibe_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NibeComponent),
            cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            # bus addresses we answer polls for (0xF9 = display card)
            cv.Optional(CONF_ADDRESSES, default=[0xF9]): cv.ensure_list(
                cv.int_range(min=0, max=0xFFFF)
            ),
            # true = pure listener: never transmit on the bus (use while the
            # real display is still attached to avoid ACK collisions)
            cv.Optional(CONF_PASSIVE, default=False): cv.boolean,
            # answer every poll with ENQ + 0x40 heartbeat frame
            cv.Optional(CONF_HEARTBEAT, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_addresses(config[CONF_ADDRESSES]))
    cg.add(var.set_passive(config[CONF_PASSIVE]))
    cg.add(var.set_heartbeat(config[CONF_HEARTBEAT]))
