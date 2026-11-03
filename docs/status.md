# nibemote status (live-verified against VVM300 + F2025 on /dev/ttyUSB1, 19200 8N1)

## Working end-to-end
- Legacy Nibe bus codec/parser (poll `<addr:u16be>`, frame `<cmd><sender:u16be><len><data><xor8>`,
  ACK 06 / NAK 15 / ENQ 05 / ETX 03), incl. self-response handling so the parser
  stays in sync when WE answer polls (our TX is not echoed to RX).
- Display takeover as 0x00F9: boot announce `40 00 f9 04 40 85 00 00 78`,
  0x22 -> 3x announce -> plain ACK -> 0x62 -> reply `62 0f` (menu text then flows).
- Menu text frames (cmd 0x50-0x53, rows 1-4, plain ASCII) received and rendered
  (virtual display in tools/menuwalk.py); e.g. `1.0 Varmvattentemperatur 51.9 C`.
- All bus frames + decoded F2025 0x65 words published to MQTT (10.42.0.174:1883)
  and live in the HA "Nibe VVM300" dashboard tab.
- Knob slider in HA (`number...knob`, 0-255, 133=center) -> `40 <pos> 00 00` frame
  sent on next poll; verified end-to-end via HA number.set_value.
- Input vocabulary (cmd 0x40, data [b0,b1,0,0]): buttons 01/02/04/08/10/20
  (b1=0x85 normal, 0x8b editing), knob b0=0x40 b1=position(wraps), heartbeat 40 85 00 00.

## Known limitation
- The master expects poll responses within ~1-2 ms; Python occasionally answers
  late -> master NAKs (`00 f9 15`) or skips (`00 f9 03`). Menu text arrives, but
  button-driven navigation is unreliable from pure Python. For reliable remote
  writes use a MCU front-end (NibeGW-style) or drive menus via the physical display.

## Menu dump
- tools/menuwalk.py: virtual display + walker, cycles Menytyp Normal/Utökad/Service,
  dumps docs/menu-dump.{json,md}. Run: display unplugged, daemon stopped,
  `uv run python tools/menuwalk.py --out docs/menu-dump.json` (power-cycle VVM300
  within the wait window to trigger the 0x22 handshake).
- Reliable alternative: reconnect the physical display, run capture.py passively,
  navigate by hand -> complete menu map from text frames.
