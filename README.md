# Nibemote — Nibe VVM300/F2025 display replacement (ESPHome)

A Waveshare ESP32-S3-Relay-1CH replaces the VVM300's display card (bus address
`0x00F9`) on the pump's internal RS485 bus — 19200 baud, Nibe 9-bit legacy
framing (not Modbus). The VVM300 keeps serving its display rows; the ESP
publishes them to Home Assistant / MQTT and a built-in web page, and sends
button and knob input back as display key events. A sniffer build (listens
only, never transmits) is included for wire debugging.

The whole exchange engine runs from the UART ISR: replies leave within
microseconds of the poll and a byte can never leak into another node's
exchange. The previous attempt (../nibemote) did this from the main loop and
bus chaos ensued; this design is the fix.

Validated live against a VVM300 + F2025: rows, menu navigation, knob (heating
curve), clock reply, F2025 telemetry sensors.

## Quick start

```bash
make env                                              # one-time: .venv from uv.lock
cp configs/secrets.yaml.example configs/secrets.yaml  # wifi credentials
make gate                                             # validate + compile + 206 sim checks

make flash-emulator                                  # production image (OTA)
make flash-sniffer                                    # passive listener (OTA)
```

First-ever flash is over USB-C (see docs/bring-up.md); `DEVICE=` overrides
the OTA address.

## Layout

| path | contents |
|---|---|
| `components/rs485_bus/` | generic ISR RS485 transport, 9-bit line format |
| `components/nibe/` | Nibe bus engine, display emulator, ESPHome entities |
| `configs/nibemote.yaml` | display emulator |
| `configs/nibemote-sniffer.yaml` | passive sniffer |
| `configs/nibemote-host.yaml` + `sim/vvm300_sim.py` | host build + simulated VVM300 for tests |
| `docs/` | protocol, command map, architecture, bring-up |

## Wiring

RS485 A/B/GND from the display port's RJ45 to the screw terminal
(TX GPIO17, RX GPIO18, EN GPIO21). Power the board from USB or its own 5 V
supply — never load the RJ45 rails.

The display is hot-swappable: put the (booted, unarmed) ESP on the bus,
unplug the physical display, flip the arm switch.

## Safety posture

- Sniffer build cannot transmit (EN pinned low, `send_burst()` refuses).
- The emulator boots silent and acts only when the arm switch is on;
  flipping it off is instant silence and the master falls back to its
  absent-node backoff.
- No parameter writes, no polling of other nodes. Reinstalling the physical
  display restores the stock system at any time.

Heating is safety-critical: read `docs/bring-up.md` before connecting
anything, and `docs/protocol.md` for the wire protocol.
