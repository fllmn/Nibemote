# nibemote ESPHome — virtual display for the Nibe F2025 + VVM300

ESPHome port of the nibemote Python project: a Waveshare
**ESP32-S3-Relay-1CH** impersonates the VVM300 display card (bus address
`0x00F9`) on the pump's internal RS485 bus, replacing the physical display
panel. All parameters are exposed as native ESPHome entities (Home Assistant
via the ESPHome integration — the old MQTT bridge is no longer needed).

Everything here was developed and tested **before the hardware arrived** using
ESPHome's host build plus a simulated VVM300 master (`sim/vvm300_sim.py`).

```
components/nibe/     ESPHome external component (C++ port of protocol.py + client.py)
  nibe.h/.cpp        legacy bus parser + poll/ENQ exchange state machine
  param_table.h      0xC0 parameter table (port of params.py)
  sensor.py          platform: sensor   (parameter: 0xNN)
  text_sensor.py     platform: text_sensor (parameter: or display_row: 1..3)
  number.py          platform: number   (parameter:, or knob: true)
  select.py          platform: select   (parameter: + options:)
  button.py          platform: button   (code: 0x01 back, 0x02 next, 0x04 enter)
esphome/
  nibemote.yaml      the real device config (ESP32-S3-Relay-1CH)
  nibemote-host.yaml host build for protocol simulation (no hardware)
  nibe-entities.yaml entity list (shared by both configs, via packages)
  secrets.yaml.example  copy to secrets.yaml, fill in wifi
sim/vvm300_sim.py    simulated VVM300 CPU (bus master) over TCP
```

## Setup

```bash
uv venv .venv-esphome --python 3.13
VIRTUAL_ENV=.venv-esphome uv pip install esphome
cp esphome/secrets.yaml.example esphome/secrets.yaml  # edit wifi
.venv-esphome/bin/esphome config esphome/nibemote.yaml   # validate
.venv-esphome/bin/esphome compile esphome/nibemote.yaml   # build
```

## Simulation (no hardware needed)

Run the firmware natively and connect the simulated VVM300 master to its
TCP UART:

```bash
.venv-esphome/bin/esphome run esphome/nibemote-host.yaml   # terminal 1
python3 sim/vvm300_sim.py                                   # terminal 2
```

The simulator plays the VVM300 CPU: boot polls, the 0x22 boot ping, the 0x62
menu request, a 0xC0 parameter dump, display text rows, a parameter write,
button presses and the knob — asserting every byte on the wire. The firmware
itself triggers a curve-offset write and button presses ~45s after boot via
its `sim_actions` script. Current status: **all 50 checks pass**, and the
firmware log shows correctly decoded sensors (e.g. `param 0x02 raw=00d9`
→ Hot Water 21.7 °C).

## Wiring (ESP32-S3-Relay-1CH ↔ VVM300 display port)

| Board | VVM300 display port |
|---|---|
| A+ | RS485 A |
| B- | RS485 B |
| GND (terminal block) | GND |

- The board's RS485 is on **GPIO17 (TX) / GPIO18 (RX)** with automatic
  direction control — no DE/RE pin needed.
- Move the board's jumper to 120R if the cable is long / communication is flaky.
- Do **not** power the board from the VVM300 display connector; use the
  board's own 5V supply (USB or screw terminal). The pump stops polling the
  bus when nothing answers as the display, so:

> **Power-cycle the VVM300 with the ESP32 attached** — otherwise the bus
> stays silent (same behaviour as with the Raspberry Pi version).

## Flashing & bringing up (when the device arrives)

1. USB-C to the laptop, then
   `.venv-esphome/bin/esphome run esphome/nibemote.yaml` (or adopt it into
   your ESPHome dashboard at 10.42.0.174:6052 and flash from there).
2. Check the board variant: if the module is N16R8, set `flash_size: 16MB`.
3. Keep the physical display connected for the first tests and set
   `nibe: passive: true` to sniff without transmitting.
4. Compare decoded sensor values against the physical display (menu 5.3.11
   for word-swap issues), then unplug the display, power-cycle the pump,
   and remove `passive`.

## Verify on hardware (TODO before first writes)

- [ ] Writable parameter sizes/ranges in `esphome/nibe-entities.yaml`
      (numbers are best-effort from the 360P forum list).
- [ ] Button codes 0x08/0x10/0x20 (semantics unknown).
- [ ] `web_server` renders the three Display Row text sensors — that's the
      "virtual display" (also available in Home Assistant).
- [ ] Whether the VVM300 pushes the F2025's 0x65 sensor block (the Python
      daemon decoded it as raw words; not yet ported).

## Safety

Writing heat pump parameters can damage the equipment. Start passive, verify
against the physical display, and only then use the number/select entities.
The relay on GPIO1 is exposed as a plain switch — leave it off.

## Design notes

- The C++ is a faithful port of `src/nibemote/protocol.py` (`LegacyParser`)
  and `src/nibemote/client.py` (`NibeClient`), including the boot announce
  (cmd 0x40 `40 85 00 00`), the 0x22 boot ping (3 announces), the 0x62 menu
  reply, NAK on bad checksums and the ENQ→ACK→frame→ACK→ETX write exchange.
- One deliberate fix: the 0xC0 value-width heuristic (which loses any 2-byte
  value whose high byte is 0x00 — every factor-10 temperature below 25.6°C)
  now consults the parameter table for known indices.
