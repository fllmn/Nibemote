# Phase-1 capture checklist

Run the sniffer with the physical display attached and capture each item
(`esphome logs configs/nibemote-sniffer.yaml | tee captures/<name>.txt`).
Mark items [x] when confirmed; note surprises in docs/hypotheses.md.

## Boot sequence (requires VVM300 power-cycle with sniffer + display present)

- [x] first activity: 0x76 storm to 41C9, EE exchange, then 0x22 → announces → 0x62 → 0x35/0x33/0x63 (captured 18:29:06)
- [x] 0x22 ping payload EMPTY (prior `85 00` disconfirmed), immediately after: panel polls get ENQ announces
- [x] announces: data `40 88 00 00`, several rapid ENQs on consecutive polls (panel power-cycle 18:28:54)
- [x] 0x62 master empty → panel replies `62 ... 01 0F` (captured 18:29:07)
- [ ] heartbeat cadence after the announce burst — not yet quantified (analyze longer captures)

## Key codes (press each button slowly on the physical panel)

- [x] Plus → `40 00 F9 04 01 89 00 00 30` — b0=0x01 ✓, flag byte 0x89 (not 0x83)
- [x] Minus → 0x02 (flag 0x89)
- [x] Enter → 0x04 (flag 0x89, chk 0x30)
- [ ] long-press/repeat behavior — not tested yet
- [x] panel ENQs on its next own poll, delivered <100 ms after the press; frame → master 06 → ETX → next poll
- [x] yes: 0x89 live-captured (prior docs' 0x83 unconfirmed on this unit)

## Rows

- [ ] rows 2/3/4 text (0x51/0x52/0x53) match what the display shows
- [ ] row 1 (0x50) payload: 3 bytes; record bit patterns vs symbols on screen
- [ ] latin-1 chars (°, å ä ö) decode correctly in the row text sensors
- [ ] 0xFF keepalives: exactly where do they sit in the exchange flow?

## Poll cycle structure

- [ ] which nodes are polled in which order (00F9, 00F1, 00F5, 41C9…), repeats per cycle
- [ ] typical interval between polls to the display (ms)
- [ ] how long after the poll does the master wait for the slave reply? Watch the
      "next own poll" timing when the display is slow (panel detached tests only —
      do NOT detach while weaning; observe clean traffic instead)
- [ ] reply latency of the physical panel (poll→05 gap in our µs byte trace)

## Other nodes (passive observation only)

- [ ] 0x65 frames from 41C9: banner bytes, plausibility of LE s16 ×÷10 temps
- [ ] 0x71/0x72/0x76/0xA1 frames from 000A: contents and recipients
- [ ] 0xD4 exchanges: who sends what; confirm the display replies nothing

## Integrity

- [ ] 10-minute capture with zero `BAD frame` warnings and zero anomalies
- [ ] gap statistics: median inter-burst gap; sanity-check `gap_symbols: 4`
- [ ] confirm shares: polls_own counts ≈ polls observed on the wire for 00F9

## After each capture session

1. `python3 tools/analyze_captures.py captures/<name>.txt`
2. Update docs/commands.md, docs/hypotheses.md and this checklist.
3. Update the simulator (sim/vvm300_sim.py) to match newly verified wire facts and
   rerun the host suite — the emulator must match reality before phase 2.
