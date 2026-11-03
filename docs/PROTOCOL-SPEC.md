# Nibe VVM300 + F2025 internal RS485 bus — protocol specification

Target hardware for the ESPHome build: ESP32-S3 + RS485 transceiver (e.g. MAX3485/MAX485
with DE/RE, or auto-direction). Reference implementation: `src/nibemote/protocol.py`
and `src/nibemote/client.py` (Python, live-verified). Everything below was captured
on a real VVM300 + F2025 on `/dev/ttyUSB1`.

## 1. Physical / link layer

- RS485, **19200 baud, 8 data bits, no parity, 1 stop (8N1)**, half-duplex.
- **Watch A/B polarity**: one USB adapter read inverted/garbled until A/B were swapped.
  If bytes look scrambled, swap A/B.
- Terminate long runs with 120 Ω at both ends.
- The display connector (RJ45 on the VVM300) also carries **+5 V / +12 V / GND**
  (per the forum thread t=13714) — the display is powered from the bus. Do not load
  these rails heavily; the VVM may detect display presence electrically.
- The bus master is the **VVM300 CPU, address 0x000A**. It polls nodes continuously
  (~0.5 s cycle) and expects a response **within ~1–2 ms** (late answers get NAK or
  are skipped — this is why an MCU front-end is recommended; Python on Linux is
  marginal).

## 2. Nodes / addresses (16-bit, big-endian on the wire)

| address | node |
|---|---|
| 0x000A | VVM300 CPU (bus master) |
| 0x00F9 | **display card** (the panel we replace) |
| 0x00F5 | relay board |
| 0x00F1 | another accessory (seen polled) |
| 0x41C9 | **F2025 heat pump** |
| 0x0014 | RCU (external display, older protocol) |

## 3. Frames

```
poll (master->node) : <addr_hi> <addr_lo>                      2 bytes
node response       : 06 ACK | 05 ENQ (has data) | 15 NAK      1 byte
data frame (either) : <cmd> <sender_hi> <sender_lo> <len> <data...> <csum>
end of exchange     : 03 ETX                                    1 byte (sent by the frame's sender)
```

- `csum` = XOR of all frame bytes from `cmd` through the last data byte.
- Exchange: master polls → node answers ACK (nothing to say) or ENQ (has data) →
  if ENQ, master ACKs 06 → node sends its frame → master ACKs 06 (or NAKs 15 on bad
  csum) → **the frame's sender** sends ETX 03.
- Verified example (live capture): `55 00 0a 02 41 23 3f` = cmd 0x55, sender 0x000A,
  len 2, data `41 23`, csum 0x3f. Text frame: `52 00 0a 15 "Varmvattentemperatur\0" 70`.

## 4. Display boot / menu handshake (as display 0x00F9)

Captured with the real panel at boot:

1. Master sends cmd **0x22** (empty).
2. On the next ~3 polls of 0x00F9, answer **ENQ + frame cmd 0x40, data `40 85 00 00`**
   (announce/heartbeat): full frame `40 00 f9 04 40 85 00 00 78`.
3. Then answer one poll with plain **ACK**; master sends cmd **0x62** (empty).
4. On the next poll answer **ENQ + frame cmd 0x62, data `0f`**: `62 00 f9 01 0f 95`.
5. Menu text frames (cmd 0x50–0x53) start flowing.

The display can also be "hot-plugged": announcing when it appears is enough; the
handshake is not strictly boot-locked.

## 5. Display content frames (VVM -> display)

- cmd **0x50/0x51/0x52/0x53** = display rows 1–4, plain ASCII, NUL-padded (~21 chars).
  Observed layout: row2 = value, row3 = label, row4 = menu number + clock.
  Examples: r2 `    51.9°C`, r3 `Varmvattentemperatur`, r4 `1.0            17:52`.
- cmd 0xFF = empty keepalive (master polls display with this most of the time).
- Other cmds seen on the bus: 0x55, 0x71, 0x72, 0x76, 0xA1, 0xC4 (VVM status),
  0x45 (relay), **0x65 from 0x41C9 = F2025 sensor block** (19 bytes = little-endian
  s16 pairs, value/10 = °C etc.), 0xEE, 0xAE/0xAF/0xA3, 0xD4, 0x33/0x35/0x63 (boot).

## 6. Input frames (display -> VVM), all cmd 0x40, data = [b0, b1, 0x00, 0x00]

- **6 buttons**: b0 = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20 (bitmask codes).
  b1 = 0x85 in normal menu mode, 0x8b while editing a value.
  Observed: 0x02 = next/forward, 0x04 = enter/select, 0x01 = back.
- **Temperature knob** ("Förskjutning värmekurva"): b0 = 0x40, b1 = absolute knob
  position 0–255 (wraps); turning sends successive positions.
- **Heartbeat/announce**: `40 85 00 00`.

## 7. Parameter read/write (0xC0) — from forum thread t=13714 (FredRovers)

- cmd **0xC0** frames carry parameter groups: data = repeated `00 <index> <value:1-2 bytes>`.
- Write: on poll, answer ENQ → master ACK → send `C0 <our_addr> <len> 00 <index> <value...> <csum>`
  → master ACK → send ETX. Example from the thread: `C0 00 14 04 00 14 01 45 <csum>`.
- Index map (360P-era, partially applicable): 0x01 outdoor temp, 0x02 hot water,
  0x06 supply, 0x07 return, 0x0A curve slope, 0x0B curve offset, 0x13 compressor on,
  0x1A starts, 0x1B compressor hours, ... (see `src/nibemote/params.py`).
- On the VVM300 the menu values were observed as display text instead; 0xC0 was not
  seen during menu navigation.

## 8. Full menu structure (VVM300 manual + live captures)

Top-level menus (manual, "Menyförklaring"):

| menu | contents |
|---|---|
| 1.0 | Varmvattentemperatur (hot water) — live: 51.9 °C |
| 2.0 | Framledningstemperatur — live: 31.1(36.8) °C; 2.1 Kurvlutning = 9, 2.2 Förskj. värmekurva = 0, 2.3 min, 2.9 Åter |
| 3.0 | Framledningstemperatur 2 |
| 4.0 | Utetemperatur |
| 5.0 | Värmepump (heat pump status) |
| 6.0 | Rumstemperatur / -inställning |
| 7.0 | Klocka |
| 8.0 | Övriga inställningar — 8.1.0 Displayinställningar, **8.1.1 Menytyp = Normal/Utökad/Service**, 8.1.4 Bakgrundsbelysning = 2, 8.1.5 Åter |
| 9.0 | Servicemenyer (installer; visible when Menytyp = Service) |

- **Three menu levels** are selected in 8.1.1 "Menytyp": Normal / Utökad / Service —
  they show/hide features (9.0 service menu requires Service).
- Display rows: r2 = value, r3 = label, r4 = `menu   clock`.
- Live captures/screens: `docs/menu-map.md`, `docs/menu-dump.md` (from `tools/menuwalk.py`).

## 9. ESPHome implementation notes

- UART 19200 8N1 + RS485 transceiver; drive DE/RE (or use auto-direction MAX3485).
  ESPHome `uart:` + a custom component modeled on `src/nibemote/client.py`.
- **Answer polls inside the UART RX loop / high-frequency component** — the ~1–2 ms
  ACK window is why the ESP32 (not the Pi's Python) should own the bus timing.
- The newer Nibe pumps (F1145/F1255, VVM310+, S-series) use a different framing
  (`5C`/`C0` start bytes, byte stuffing) — see `protocol.py` `encode_master`/`FrameParser`
  and elupus/esphome-nibe. This spec is the **legacy VVM300/F2025** variant.
- Publish states to MQTT (`nibemote/...`), HA discovery under `homeassistant/...`
  (see `mqtt_bridge.py`); broker used live: 10.42.0.174:1883.
