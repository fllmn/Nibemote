# nibemote — ESPHome virtual display for the Nibe F2025 + VVM300

A Waveshare **ESP32-S3-Relay-1CH** replaces the VVM300's display panel on the
pump's internal RS485 bus (19200 baud 8N1, legacy Nibe protocol, see
[docs/PROTOCOL-SPEC.md](docs/PROTOCOL-SPEC.md)): it impersonates the display
card (bus address `0x00F9`), reads/writes parameters and renders the display's
text rows, exposing everything to Home Assistant as native ESPHome entities
(web server + ESPHome API — no MQTT bridge needed).

The protocol was reverse engineered in the Elektronikforumet thread
["PC-styrning av Bergvärmepump"](https://elektronikforumet.com/forum/viewtopic.php?t=13714)
and verified live on a VVM300 + F2025. An earlier Python/Raspberry Pi
implementation existed in this repo; it has been removed — the ESPHome C++
component is its successor (the git history keeps both the Python code and
the hardware captures in `docs/`).

## Layout

| path | contents |
|---|---|
| `components/nibe/` | ESPHome external component (parser, hub, entities) |
| `esphome/` | device configs, entity list, [README](esphome/README.md) |
| `simulator/` | simulated VVM300 bus master for host-build testing |
| `docs/` | live-verified protocol spec, menu map, bus captures |

## Quick start

```bash
# tooling
uv venv .venv-esphome --python 3.13
VIRTUAL_ENV=.venv-esphome uv pip install esphome
cp esphome/secrets.yaml.example esphome/secrets.yaml   # edit wifi

# test the protocol against the simulated VVM300 (no hardware needed)
.venv-esphome/bin/esphome run esphome/nibemote-host.yaml   # terminal 1
python3 simulator/vvm300_sim.py                            # terminal 2

# build / flash the real firmware (ESP32-S3-Relay-1CH)
.venv-esphome/bin/esphome run esphome/nibemote.yaml
```

See **[esphome/README.md](esphome/README.md)** for wiring, bring-up and the
hardware verification checklist, and
[docs/PROTOCOL-SPEC.md](docs/PROTOCOL-SPEC.md) for the bus protocol.

## Safety

Writing heat pump parameters can damage the equipment. Start read-only
(`passive: true` with the physical display still attached), verify against
the physical display, and only then use the writable entities. The VVM300
stops polling the bus when nothing answers as the display — power-cycle it
with the ESP32 attached.
