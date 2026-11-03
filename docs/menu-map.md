# VVM300 menu map — captured live from the display bus (ttyUSB1)

Captured by walking the real display through the menus while nibemote sniffed
passively (`/tmp/nibe_menu.log`). Display text frames: cmd 0x50-0x53 = rows 1-4,
plain ASCII; row2 = value, row3 = label, row4 = menu number + clock.

## Screens seen
| menu | label (row3) | value (row2) |
|---|---|---|
| 1.0 | Varmvattentemperatur | 51.6 °C (later 31.1(36.8) °C on 2.0) |
| 2.0 | Framledningstemp. | 31.1(36.8) °C (actual(setpoint)) |
| 2.1 | Kurvlutning | 9 |
| 2.2 | Förskj. värmekurva | 0 |
| 2.9 | Åter | |
| 8.0 | Övriga inställningar | |
| 8.1.0 | Displayinställningar | |
| 8.1.1 | Menytyp | Normal / @Service@ / @Utökad@ (selectable) |
| 8.1.4 | Bakgrundsbelysning | 2 |
| 8.1.5 | Åter | |

## Display -> VVM button frames
cmd 0x40, sender 0x00F9, data = [button, 0x85, 0x00, 0x00]:
- `40 85 00 00` — heartbeat/idle (x17)
- `02 85 00 00` — navigate/plus (x18)
- `04 85 00 00` — enter/select (x7)
- `01 85 00 00` — back/minus (x5)

Boot announce: `40 00 f9 04 40 85 00 00 78` (cmd 0x40 data `40 85 00 00`).

## Notes
- No 0xC0 parameter frames appear during menu navigation — values are pushed
  to the display as text rows. Reading settings = scrape text frames.
- Writing = send button frames to navigate + change + enter, or direct 0xC0
  param write (index map still to be confirmed).
- Main menus (from VVM300 manual): 1.0 varmvatten, 2.0 framledning (2.1 kurvlutning,
  2.2 förskjutning, 2.3 min), 3.0 framledning 2, 4.0 utetemp, 5.0 värmepump,
  6.0 rumstemperatur, 7.0 klocka, 8.0 övrigt (8.1.x display), 9.0 service (needs
  Menytyp=Service in 8.1.1).

## Full input vocabulary (captured 13:56, display connected, all buttons + knob)

All display->VVM input frames: cmd 0x40, sender 0x00F9, data = [b0, b1, 0x00, 0x00].

- 6 buttons, b0 = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20 (bitmask codes).
  b1 = 0x85 in normal menu mode, 0x8b while editing a value.
  Observed semantics: 0x02 = forward/plus, 0x04 = enter/select, 0x01 = back (others TBD).
- Temperature knob: b0 = 0x40, b1 = absolute knob position (wraps 0x00-0xff);
  turning sends successive positions (e.g. 61 67 6a 6f ... one way, ... ff e2 94 8b the other).
- Heartbeat / boot announce: b0 = 0x40, b1 = 0x85 (i.e. `40 85 00 00`).
