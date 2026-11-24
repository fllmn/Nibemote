# Commands and registers

Command and register reference for the internal bus. Wire behavior and
frame grammar: [protocol.md](protocol.md). Entries marked *unknown* are
listed as observed; decoding notes and next experiments live in
[hypotheses.md](hypotheses.md).

## Keys and control bytes (cmd 0x40, panel -> master)

data = `[key, <relay>, 00, 00]`. The content request uses key id 0x40 with
the same four-byte shape. `<relay>` is a slowly varying relaid system
value.

| id | key |
|---|---|
| 0x01 | plus |
| 0x02 | minus |
| 0x03 | knob (data[1] = placement 0-255, vertical = 127) |
| 0x04 | enter |

## Display slot 0x00F9

| cmd | direction | payload | meaning |
|---|---|---|---|
| 0xFF | master -> panel | empty | idle-slot keepalive (~40% of idle slots) |
| 0x40 (data0=0x40) | panel -> master | `40 <relay> 00 00` | content request (repeat phase) |
| 0x40 (data0=key id) | panel -> master | `[key, <relay>, 00, 00]` | key / knob event |
| 0x50 | master -> panel | 3-byte bitmap | row 1 symbol strip (bits unmapped) |
| 0x51 / 0x52 / 0x53 | master -> panel | latin-1 text | rows 2 / 3 / 4 |
| 0x62 | master -> panel | empty | menu session opener |
| 0x62 | panel -> master | `0F` | menu session reply |
| 0x63 | master -> panel | `0F` | menu follow-through, unknown |
| 0x32 / 0x33 / 0x35 | master -> panel | `00` / empty | one-shot notification; the panel receipts it, no answer frame |
| 0x32 | panel -> master | `00` | the panel sent it; the master receipts it and does not answer (hypotheses.md 12) |
| 0x72 / 0x76 | both | `05 <reg>` / `05 <reg> <val>` | register read / write |
| 0xD4 | master -> panel | empty | clock query (7-byte BCD answer) |

## Relay board 0x00F5

| cmd | direction | payload | meaning |
|---|---|---|---|
| 0x55 | master -> relay | 2 bytes | relay control/state, unknown |
| 0x45 | relay -> master | `01` | state report, unknown |
| 0x72 / 0x76 | both | `05 <reg>` / `05 <reg> <val>` | register read / write |

## F2025 0x41C9

| cmd | direction | payload | meaning |
|---|---|---|---|
| 0x22 | master -> F2025 | empty | boot link check |
| 0x65 | master push -> answer | probe push -> 19 bytes | sensor block (words below) |
| 0x71 | master push -> answer | empty push -> 16 bytes | status block (fields below) |
| 0x72 / 0x76 | both | `05 <reg>` / `05 <reg> <val>` | register read / write |
| 0xC4 / 0xC5 | master -> F2025 | empty | one-shot queries |
| 0xEE | master -> F2025 | empty -> `00 78` | query/response, unknown |

## Accessory 0x00F1

| cmd | direction | payload | meaning |
|---|---|---|---|
| 0xA1 | master -> 00F1 | 1 byte, incrementing | control/status value |
| 0xA3 / 0xAF | master -> 00F1 | `02` | one-shots |

## F2025 sensor words (cmd 0x65, 19-byte answer)

Nine LE s16 words plus pad, value/10 = °C.

| word | meaning |
|---|---|
| 0 | outdoor air temperature (at the heat pump) |
| 1 | FÖRÅNGARTEMPERATUR — evaporating |
| 2 | SUGGASTEMPERATUR — suction gas |
| 3 | VÄTSKELEDNINGSTEMPERATUR — refrigerant liquid line into the valve |
| 4 | HETGASTEMPERATUR — hot gas out of the compressor (> ~45 °C = compressor running) |
| 5 | constant 16384 |
| 6 | non-thermal blob (unmapped) |
| 7 | constant -32767 |
| 8 | water return temperature |

Refrigerant loop: outdoor -> evaporating -> suction gas -> compressor ->
hot gas -> liquid line. The water side exposes the return temperature only.

## F2025 status block (cmd 0x71, 16-byte answer)

Byte-packed u16 LE fields on odd offsets (no word grid).

- bytes 7-8: compressor operating hours (u16 LE).
- bytes 3-4: compressor starts candidate (unmapped).

## Register scheme (cmd 0x72 read, cmd 0x76 write)

`05 <reg>` reads, `05 <reg> <val>` writes. The master addresses the same
register numbers across nodes with node-specific values. Power-up writes
to the F2025 arrive as 0x76 len-3 bursts.

Registers are **master-initiated**: the master asks `72 05 <reg>` and the addressed
node answers in the same window — the display with `05 <reg> <val>` (e.g. `05 40 06`),
the F2025 with a single value byte (e.g. `72 14` for register `0x11`). nibemote logs
each answer raw, attributed to the register the master asked for that node
(hypotheses.md 8).

A panel-initiated read is inert: `05 11` and `05 40` were sent from the panel and the
master receipt-ACKed both without answering. The register-address number and
"Send 0x72 read" button are experiment tools, not a read channel.

## The panel's own block (0x40–0xBF)

The master sweeps 128 registers from the panel at startup, one read per exchange. The
real display answers every one as `72 05 <reg> <val>`, and the values are constant
across every capture. nibemote answers the same way: the receipt slot carries ENQ, the
grant the frame (in-window, then the panel closes).

Non-zero cells (everything else is `00`):

```
40=06 44=68 45=77 46=D7 48=01 4C=06 50=68 51=77 52=D7 54=01
58=13 5C=66 5D=77 5E=D7 B8=01 BC=D1 BD=D0 BE=E0 BF=4B
```
