import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.rs485_bus import RS485Bus
from esphome.const import CONF_ADDRESS, CONF_ID

CODEOWNERS = ["@mans"]
DEPENDENCIES = ["rs485_bus"]
MULTI_CONF = True

nibe_ns = cg.esphome_ns.namespace("nibe")
NibeBus = nibe_ns.class_("NibeBus", cg.Component)

CONF_ROLE = "role"
CONF_RS485_BUS_ID = "rs485_bus_id"
CONF_NIBE_ID = "nibe_id"
CONF_MENU_REPLY = "menu_reply"
CONF_NIBE_LOG_LEVEL = "log_level"
CONF_DISPLAY_WEB_PORT = "display_web_port"
CONF_D4_REPLY = "d4_reply"
CONF_D4_TAIL = "d4_tail"
CONF_D4_TIME_ID = "d4_time_id"
CONF_REQUEST_BYTE = "request_byte"

ROLE_PASSIVE = "passive"
ROLE_EMULATOR = "emulator"


def _role(value):
    cv.check_not_templatable(value)
    value = cv.string(value)
    if value not in (ROLE_PASSIVE, ROLE_EMULATOR):
        raise cv.Invalid(f"role must be '{ROLE_PASSIVE}' or '{ROLE_EMULATOR}'")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NibeBus),
            cv.Required(CONF_RS485_BUS_ID): cv.use_id(RS485Bus),
            # sniffer: captures only; emulator: answers polls, requests rows, replies
            # 0x62, delivers queued keys (protocol.md) — requires active: true, and a
            # passive transport is refused loudly at setup
            cv.Optional(CONF_ROLE, default=ROLE_PASSIVE): _role,
            cv.Optional(CONF_ADDRESS, default=0x00F9): cv.int_range(min=0, max=0xFFFF),
            cv.Optional(CONF_MENU_REPLY, default=True): cv.boolean,
            # NOTE: all emulator timings are FIXED wire-measured C++ constants (nibe_bus.h):
            # poll 2400us, grant 160us, gap 180us, close 670us, push_ack 15.4ms, burst/breath 2/2, no-poll 30s
            cv.Optional(CONF_DISPLAY_WEB_PORT, default=0): cv.int_range(min=0, max=65535),
            cv.Optional(CONF_D4_REPLY, default=False): cv.boolean,
            cv.Optional(CONF_D4_TAIL, default=0): cv.int_range(min=0, max=0xFF),  # 0 = auto: day-of-week
            cv.Optional(CONF_D4_TIME_ID): cv.use_id("time.RealTimeClock"),
            # request/key data[1]: the relaid system value (commands.md) — set to the observed byte for probes
            cv.Optional(CONF_REQUEST_BYTE, default=0x88): cv.int_range(min=0, max=0xFF),
            # 0 = 10s summary, 1 = + one line per exchange, 2 = + per-event wire detail
            cv.Optional(CONF_NIBE_LOG_LEVEL, default=1): cv.int_range(min=0, max=2),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    # d4_reply without d4_time_id: host epoch = real time; bare ESP32 = 1970 (refused by the validity gate)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[CONF_RS485_BUS_ID])
    cg.add(var.attach_to(bus))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_emulator_role(config[CONF_ROLE] == ROLE_EMULATOR))
    cg.add(var.set_menu_reply(config[CONF_MENU_REPLY]))
    if config[CONF_DISPLAY_WEB_PORT]:
        cg.add(var.set_display_web_port(config[CONF_DISPLAY_WEB_PORT]))
        cg.add_define("USE_NIBE_DISPLAY_WEB")  # compiles nibe_web.cpp in
    cg.add(var.set_d4_reply(config[CONF_D4_REPLY]))
    cg.add(var.set_d4_tail(config[CONF_D4_TAIL]))
    if CONF_D4_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_D4_TIME_ID])
        cg.add(var.set_time_source(clock))
    cg.add(var.set_request_byte(config[CONF_REQUEST_BYTE]))
    cg.add(var.set_log_level(config[CONF_NIBE_LOG_LEVEL]))
