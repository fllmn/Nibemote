# Nibe VVM300/F2025 internal bus

A polled master/slave protocol on RS485 half duplex, 19200 baud. Wire chars
carry 8 data bits plus a 9th marking bit (address-marked, MDB style): poll,
ENQ and the master's close ETX carry 1 in the 9th slot, all frame and reply
bytes carry 0 — chars are 11 bit-times (~573 us). A plain 8N1 device
interoperates: it reads the 9th slot as the stop bit (an ESP32-S3 does so
natively; other receivers may see per-byte framing errors, harmless). The
VVM300 CPU drives every exchange; all other nodes speak when polled.
Command and register reference: [commands.md](commands.md). The probe and
marking table: [hypotheses.md](hypotheses.md).

| address | node |
|---|---|
| 0x000A | VVM300 CPU — the bus master |
| 0x00F9 | display card (this project emulates it) |
| 0x00F5 | relay board |
| 0x00F1 | accessory |
| 0x00FA | boot-scan target |
| 0x41C9 | F2025 heat pump |

## Grammar

```
poll (master->node)  : <addr_hi> <addr_lo>
node reply to poll   : 06 ACK (nothing to say) | 05 ENQ (has a frame)
grant                : master 06, answering a node's ENQ
data frame           : <cmd> <sender_hi> <sender_lo> <len> <data...> <csum>
csum                 : XOR-8 over cmd .. last data byte
receipt ACK          : 06 from the receiver, after a frame's checksum
NAK                  : 15 from the master, for a frame it sliced but whose checksum failed
close                : 03 ETX, written by the frame's sender
```

Bytes are paced: ~650 µs per byte on the wire, 110-285 µs idle between a
node's own frame bytes. Address bytes also occur inside frame payload, so
a poll is only a poll at the exchange boundary.

The display port (RJ45) carries +5 V / GND on its unused pairs: the panel
is bus powered.

## Exchange semantics

- Poll -> node replies 06 (idle) or 05 (has a frame).
- ENQ -> master 06 grant -> node sends its frame, master receipt-ACKs,
  node closes with 03 (~670 µs after the receipt).
- Master pushes into the node's slot: the display receipt-ACKs each push
  ~15.4 ms after its checksum; the master closes its pushes with 03.
- Idle exchange: poll -> panel 06 -> master 03 (~650 µs later).
- The master's listener opens ~1-2.5 ms after its own bytes leave the
  wire; timed replies land in that window (panel poll answers at +0.8-2.4
  ms; granted frames start ~+160 µs after the grant).
- The master answers a frame it could slice but whose checksum failed with NAK
  `15`; a frame whose bytes cannot be sliced gets no answer. ETX `03` in place
  of an answer means the command was not understood.
- Poll cadence: an active display slot ~9.2 ms (~28/s); a silent slot
  ~520 ms.
- Reply timing constants live in `components/nibe/nibe_bus.h`.

## The display slot

1. Power-up: the panel answers its polls with ENQ carrying the content
   REQUEST `40 <relay> 00 00`, repeated until the master delivers the first
   content row. The master then pushes the rows into that same slot: panel
   reply byte, master frame, panel receipt `06`, master close `03`.
2. Menu session: the master sends 0x62 (empty); the panel answers `0F`.
3. Content: 0x50 = row 1 symbol strip, 0x51/0x52/0x53 = rows 2-4 (latin-1
   text, NUL padded, 0xB0 = °), repainted every ~5.5 s. The request phase
   ends at the first content row.
4. Idle: 06/03 each poll; 0xFF keepalive (empty) in ~40% of idle slots.
5. Keys leave as cmd 0x40 frames `[key, <relay>, 00, 00]`:
   plus=0x01, minus=0x02, enter=0x04; the knob event carries its
   placement (0-255) in data[1], id 0x03.
6. Presence is answering polls. A panel that returns is re-detected
   within one ~520 ms backoff poll and re-runs the request cycle; the
   master needs no reboot.

## The clock

The master queries 0xD4 every ~64 s. The panel owns the system clock and
answers with 7 bytes BCD `yy mm dd hh mm ss <day-of-week>` (Sunday=1);
row 4 shows this clock. Answering is opt-in in the emulator (see
`d4_reply`) and uses NTP-synced time.
