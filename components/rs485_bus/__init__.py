import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_BAUD_RATE, CONF_ID, CONF_RX_PIN, CONF_TX_PIN, CONF_UART_ID

CODEOWNERS = ["@mans"]
MULTI_CONF = True

rs485_ns = cg.esphome_ns.namespace("rs485_bus")
RS485Bus = rs485_ns.class_("RS485Bus", cg.Component)

CONF_EN_PIN = "en_pin"
CONF_UART_NUM = "uart_num"
CONF_GAP_SYMBOLS = "gap_symbols"
CONF_ACTIVE = "active"
CONF_RX_BUFFER = "rx_buffer"


def _validate(config):
    hardware = {k for k in (CONF_RX_PIN, CONF_TX_PIN, CONF_EN_PIN) if k in config}
    if CONF_UART_ID in config:
        if hardware:
            raise cv.Invalid(
                "rs485_bus: use either hardware pins (rx/tx/en) or uart_id (stream mode), not both"
            )
        return config
    if hardware != {CONF_RX_PIN, CONF_TX_PIN, CONF_EN_PIN}:
        raise cv.Invalid(
            "rs485_bus hardware mode requires all of rx_pin, tx_pin and en_pin "
            "(or switch to stream mode with uart_id)"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RS485Bus),
            cv.Optional(CONF_RX_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_TX_PIN): pins.internal_gpio_output_pin_schema,
            # DE/+RE tied line: HIGH while transmitting on the Waveshare board (LOW = receive); internal GPIO
            cv.Optional(CONF_EN_PIN): pins.internal_gpio_output_pin_schema,
            # hardware mode claims an ESP32-S3 UART peripheral (never esphome uart:); UART0 is the logger —
            # the board's RS485 on GPIO17/18 routes to UART1
            cv.Optional(CONF_UART_NUM, default=1): cv.int_range(min=1, max=2),
            cv.Optional(CONF_BAUD_RATE, default=19200): cv.positive_int,
            # RX-idle gap threshold in byte-times (19200: ~0.52 ms/unit); 4 units ≈ this bus's inter-frame rhythm
            cv.Optional(CONF_GAP_SYMBOLS, default=4): cv.int_range(min=2, max=31),
            # false = transceiver locked to receive (sniffer-safe default); the emulator sets true explicitly
            cv.Optional(CONF_ACTIVE, default=False): cv.boolean,
            # raw trace ring entries (power of two)
            cv.Optional(CONF_RX_BUFFER, default=8192): cv.All(
                cv.positive_int, lambda v: v & (v - 1) == 0
            ),
            # stream mode: attach to an esphome uart: id (host tcp_uart or task-mode) instead of owning a peripheral
            cv.Optional(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_UART_ID in config:
        uartc = await cg.get_variable(config[CONF_UART_ID])
        cg.add(var.set_uart(uartc))
        cg.add_define("USE_RS485_BUS_STREAM")
    else:
        for key, setter in (
            (CONF_RX_PIN, "set_rx_pin"),
            (CONF_TX_PIN, "set_tx_pin"),
            (CONF_EN_PIN, "set_en_pin"),
        ):
            pin_expr = await cg.gpio_pin_expression(config[key])
            cg.add(getattr(var, setter)(pin_expr))
        cg.add(var.set_uart_num(config[CONF_UART_NUM]))
    cg.add(var.set_baud_rate(config[CONF_BAUD_RATE]))
    cg.add(var.set_gap_symbols(config[CONF_GAP_SYMBOLS]))
    cg.add(var.set_active(config[CONF_ACTIVE]))
    cg.add(var.set_rx_buffer(config[CONF_RX_BUFFER]))
