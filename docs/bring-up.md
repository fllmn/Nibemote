# Bring-up guide (Waveshare ESP32-S3-Relay-1CH ↔ VVM300 display port)

## Board facts (verified against the wiki + prior schematic check)

| function | GPIO |
|---|---|
| RS485 TX (via isolator → SP3485) | 17 |
| RS485 RX | 18 |
| RS485_EN (DE + /RE tied) | **21** — LOW = receive, HIGH = drive |
| Relay (leave off) | 1 |
| BOOT | 0 |

- GPIO21 (RS485_EN) is undocumented by Waveshare and may float during ROM
  boot / bootloader / flash / brownouts; a floating-high EN would drive the
  bus continuously (A↔B ≈ 0 Ω). A 10 kΩ pull-down is cheap insurance — but
  note our board ran without one for weeks on the live bus and never froze it.
- Power: USB-C at the laptop for capture work. Permanent install: the RJ45 display
  leads carry +5 V (and reportedly +12 V) — the prior repo advises against loading
  those rails; prefer the board's own 5 V supply or a small USB supply. If you do
  use the VVM300's +12 V lead, feed it to the board's 7–36 V screw terminal only
  (never the 5 V/USB path) and double-check with the wiki rev of your board.
- The RJ45 pinout for A/B/GND/+5/+12 is in the Elektronikforumet thread; verify
  with a multimeter before injecting any voltage.
- Termination: leave the board's 120 Ω jumper off unless the cable is long and the
  capture shows errors.

## Firmware

```bash
# tooling (one-time)
make env                                                # provision .venv from pyproject/uv.lock

cp configs/secrets.yaml.example configs/secrets.yaml   # wifi credentials

# phase 1 — passive sniffer (NEVER transmits)
make flash-sniffer                                      # network OTA (DEVICE=... override)
# first-ever USB-C flash: .venv/bin/esphome run configs/nibemote-sniffer.yaml

# host protocol tests (no hardware)
make sim                                                # → ALL CHECKS PASSED
```

## Phase 1 — capture with the physical display attached (SAFE)

Goal: confirm the open items in docs/hypotheses.md using our own captures
before the emulator is allowed on the bus.

1. Flash the sniffer build; confirm boot logs show `active: False`.
2. Wire **A↔VVM A, B↔VVM B, RS485 GND↔display-port GND** with the physical display
   still connected (the sniffer listens in parallel; EN is held low).
3. Power the board first, watch `esphome logs`; then power-cycle the VVM300 to
   capture its boot sequence (0x22 ping, announces).
4. Work the checklist in docs/capture-checklist.md while `tee`-ing the log.
5. Analyze: `python3 tools/analyze_captures.py capture.txt`.
6. Calibrate: the "Last bus gap" sensor shows true inter-burst silences; adjust
   `rs485_bus: gap_symbols` if exchanges are being split or merged wrongly.

Expect to see in the log: polls to 00F9/00F1/00F5/41C9, the real panel's ENQs +
announce frames (sender 00F9), key frames when you press panel buttons (these prove
the key bytes), rows (cmd 50..53) — the sniffer's text sensors mirror rows 2–4 live.

## Phase 2 — become the display (HOT SWAP)

Displays can be swapped with the master running — probe-fa-2 (2026-11-14)
proved a running VVM300 drops a vanished panel to its ~520 ms absent-node
backoff and re-detects a fresh one within a single backoff poll. No VVM300
power-cycle, no announce window to catch.

1. Freeze the captures; graduate settled items from docs/hypotheses.md into
   docs/protocol.md (behavior) or docs/commands.md (commands/registers); rejected
   ones stay in hypotheses.md with their rejection note.
2. Rerun the host sim against any engine change (`esphome run
   configs/nibemote-host.yaml` + `python3 sim/vvm300_sim.py` — must end
   `ALL CHECKS PASSED`).
3. Flash configs/nibemote.yaml (`role: emulator`, `active: true`).

   It boots UNARMED and fully silent, so it may sit on the bus beside the
   physical panel — power it whenever, even before the panel is out.
4. **Unplug the physical display.** The master backs off within one missed ACK.
5. Flip the "Enable display emulation" switch: the first backoff poll gets
   our ENQ + `40 67 00 00`, the request storm repeats until the first row
   arrives (sub-second on a running master), rows land, entities update —
   the ESP is now the display. Verify in the logs: `ctrl 05 (slot 00F9)` →
   `06` grant → `tx: 40` deliveries → `tx: 06` bare ACKs at idle.
6. Drive the menu with the Plus/Minus/Enter buttons from Home Assistant. Verify on
   the VVM300 (listen for relay clicks / check a sensor value changing) — one
   action at a time, watching the anomaly counter.
7. If anything looks wrong: flip the arm switch off (the master falls straight
   back to its absent-node backoff), or reflash the sniffer, reinstall the
   physical display, and capture. The heating tolerates a missing display far
   better than a lying one. Recovery after a physical-panel reinstall needs
   no reboot either — same grammar in reverse.

## If the heat stops

Reinstall the physical display and power-cycle the VVM300 — that immediately
restores the stock system (verified by the prior project). Then compare the
sniffer captures of the failure window (the firmware logs every anomaly), and
remember the rule: **silence is safe, unauthorized bytes are not.**

## Known things nobody should do

- Don't enable `role: emulator` while the physical display is attached (two devices
  answer polls for 0x00F9 → collisions).
- Don't touch parameter writes (cmd 0xC0) — not implemented on purpose.
- Don't answer the 0xD4 exchanges.
- Don't poll other nodes (F2025 etc.) from the ESP — the emulator never initiates.
