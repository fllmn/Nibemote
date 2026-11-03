# nibemote — Nibe F2025 + VVM300 RS485 remote controller

Replaces the VVM300 display panel: a Raspberry Pi 3B wired to the VVM300 over a
RS485 adapter (19200 baud, 8N1) impersonates the display/RCU on the Nibe
internal bus, reads/writes **all** heat pump registers, and exposes them to
Home Assistant over MQTT (MQTT discovery).

The protocol was reverse engineered in the Elektronikforumet thread
["PC-styrning av Bergvärmepump"](https://elektronikforumet.com/forum/viewtopic.php?t=13714)
(FredRovers' spec) and cross-checked against the open-source NibeGW (openHAB),
[esphome-nibe](https://github.com/elupus/esphome-nibe) and the Python
[`nibe`](https://github.com/yozik04/nibe) library.

## Bus protocol — confirmed live on a VVM300 + F2025 (legacy variant of t=13714)

RS485, **19200 8N1**, half duplex, plain 8-bit bytes (no 9th bit on this
hardware). The VVM300 CPU is the bus master and polls nodes; a polled node
must answer **within a few ms**:

```
poll           : <addr:u16be>                      (2 bytes)
node response  : 06 ACK | 05 ENQ (has data) | 15 NAK
data frame     : <cmd> <sender:u16be> <len> <data...> <xor8 csum>
end of exchange: 03 ETX
```

Verified against live capture, e.g. `55 00 0a 02 41 23 3f` (cmd 0x55, sender
0x000A = VVM300 CPU, len 2, data `41 23`, csum 0x3f) and the display text frame
`52 00 0a 15 "Varmvattentemperatur\0" 70`.

* Parameter frames use cmd `0xC0`, data = repeated `00 <index> <value:1-2B>`
  groups (FredRovers' index list in thread t=13714; see `params.py`).
* Display text lines use cmd `0x51`/`0x52`/`0x53` (rows 1-3).
* Addresses seen on a VVM300+F2025: `0x000A` VVM300 CPU (master), `0x00F5`
  relay board, **`0x00F9` display card (what we impersonate)**, `0x41C9` F2025
  heat pump.
* To write parameter *I*: answer the poll to our address with ENQ `05`, wait
  for the master's `06`, send `C0 00 F9 <len> 00 <I> <value> <csum>`, then `03`
  after the master's ACK.

> The newer F/S-series pumps (F1145, VVM310+, S-series) use a different
> framing (`5C`/`C0` start bytes, byte stuffing) — that codec is also in
> `protocol.py` (`encode_master`/`FrameParser`) for those models.

**Important:** with the display panel unplugged the VVM300 stops polling the
bus (observed: total silence). Power-cycle the VVM300 with the Pi attached —
`nibemote run` answers the boot polls (`0xF9`) and the parameter stream starts.

## Register map

`src/nibemote/data/vvm_registers.json` — 884 registers (466 writable), the
VVM225/VVM320/VVM325 map from the `nibe` library, the closest published map
for VVM300 + F2025. Registers that don't exist on the F2025 simply time out
and are skipped. Values are little-endian; set `NIBEMOTE_WORD_SWAP=true` if
32-bit values look wrong (match service menu 5.3.11 on the pump).

## Setup

```bash
uv sync                 # create .venv and install deps
cp .env.example .env    # edit serial port, MQTT broker, addresses
uv run nibemote --help
```

On the Pi:

```bash
sudo usermod -aG dialout $USER   # serial access
uv run nibemote listen           # passive sniff (panel still connected)
uv run nibemote scan             # see which addresses the VVM300 polls (0xF9 = display)
# .env: NIBEMOTE_ACK_ADDRESSES=0xF9
# unplug the display panel, power-cycle the VVM300, then:
uv run nibemote run              # daemon: answers display polls, publishes params to MQTT
uv run nibemote params           # collect 0xC0 parameter frames once
uv run nibemote write 0x0B -2    # write curve offset (validated against params.py)
```

Install as a service: copy `systemd/nibemote.service` to `/etc/systemd/system/`,
point `WorkingDirectory`/`EnvironmentFile` at the checkout, `systemctl enable --now nibemote`.

## CLI

| command | description |
|---|---|
| `nibemote run` | daemon: ACK the display address, poll all registers, publish MQTT + HA discovery |
| `nibemote listen` | passive bus sniffer, prints frames |
| `nibemote scan` | discover polled addresses / pushed registers |
| `nibemote read 40004` | one-shot register read (decimal or 0x) |
| `nibemote write 47011 21.5` | one-shot write (engineering units or enum label) |
| `nibemote list-registers [-w]` | dump the register map |

## Home Assistant

With `NIBEMOTE_HA_DISCOVERY=true` the daemon publishes discovery configs under
`homeassistant/...` — read-only registers become sensors (with proper
device/state classes), writable ones become `number` / `select` / `switch`.
State topics: `nibemote/<register>/state` (retained), command topics
`nibemote/<register>/set`.

## Wiring notes

* VVM300 display port: RS485 A/B (+ GND; some ports also supply 5/12 V — do not
  load it heavily). Many USB/RS485 adapters work; if yours needs manual
  direction control, `NIBEMOTE_RS485_RTS=true` drives RTS (default). For HATs
  with auto direction, set it to `false`.
* Terminate the bus with 120 Ω at both ends if the cable is long.

## Safety

Writing heat pump parameters can damage the equipment. Start read-only
(`listen`/`scan`/`read`), verify against the physical display, and only then
enable writes.

## Development

```bash
uv sync --extra dev
uv run pytest
```

## Menu dump test app (tools/menuwalk.py)

Virtual display + full menu walker. Requires the port free and the physical
display unplugged:

```bash
# stop the daemon if running
pkill -f "nibemote run"   # or stop the background process
# unplug the display cable from the VVM300, then:
uv run python tools/menuwalk.py --out docs/menu-dump.json
# NOW power-cycle the VVM300 (boot handshake), watch the virtual display.
# Dumps docs/menu-dump.json + docs/menu-dump.md, cycling Menytyp
# Normal/Utökad/Service (manual menu 8.1.1).
```

Manual top-level menus (VVM300, rorjour.se PDF): 1.0 varmvatten, 2.0 framledning,
3.0 framledning 2, 4.0 utetemp, 5.0 värmepump, 6.0 rumstemperatur, 7.0 klocka,
8.0 övrigt (8.1.x displayinställningar, 8.1.1 Menytyp), 9.0 service.
