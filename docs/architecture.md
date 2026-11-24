# Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ ESPHome entity layer                                                │
│   text_sensor (rows, key, F2025 hex)   sensor (values, counters)     │
│   button (keys, re-announce, read register, 0x32 query)   number     │
│   binary_sensor (polled) — plain HA entities over the sink lists     │
├─────────────────────────────────────────────────────────────────────┤
│ components/nibe — engine (split by ADR-0001)                        │
│   nibe_wire    ExchangeParser: boundaries, checksum, len-lie rescue  │
│   nibe_queries Query units + QueryScheduler: repeat refill and the   │
│                poll-reply policy (ENQ iff deliverable, 2/2 governor) │
│   nibe_codec   pure decode/encode + passive 0x72 request/answer      │
│   nibe_screen  ScreenState: rows, symbol strip, knob, content latch  │
│   nibe_web     virtual display page (reads ScreenState)              │
│   nibe_bus     hub — interrupt side: wire ring capture (byte + ts + ninth │
│                bit, gaps), slot tracker (poll pair, grant, close, frame    │
│                end), timed emission. Task: wire-ring drain, sink handlers,│
│                loop()                                                      │
├─────────────────────────────────────────────────────────────────────┤
│ components/rs485_bus — generic RS485 transport (protocol-blind)      │
│   ESP32-S3 ISR: per-byte RX, hw rx-timeout gaps, EN assert/drop      │
│   around bursts (TX_DONE), hw RS485 echo mute, raw byte trace ring,  │
│   stats. Stream mode: the same engine hooks fed from an esphome      │
│   `uart` (host builds / sim)                                        │
├─────────────────────────────────────────────────────────────────────┤
│ UART1 @ 19200 8N1, GPIO17 TX / GPIO18 RX / GPIO21 EN (Waveshare)     │
└─────────────────────────────────────────────────────────────────────┘
```

## What stays on the interrupt side (and what left)

ADR-0001 splits the engine: the interrupt side ingests and emits, the task interprets. The bus
gives 0.8–2.4 ms to answer a poll and ~160 µs to start a granted frame, and both the
ninth bit and the silence that ends a burst are only available at byte time — so the
interrupt path keeps:

- **capture**: every byte with its timestamp and ninth bit, and a gap marker from the
  hardware rx-timeout, pushed into a wire ring;
- **wire mechanics**: a small slot tracker that knows a poll pair, our slot, the grant,
  the close and where a frame ends. It answers only from those positions, so payload
  bytes can never mimic a poll. On hardware a *marked* byte is a boundary token by
  protocol fact, which also resyncs the tracker after a length lie;
- **emission**: the delayed reply byte, paced frame delivery and the closing ETX —
  armed by the RX ISR, fired by the 125 µs esp_timer tick (`loop()` on host builds).
  `send_burst` stays the transport's only way onto the wire; the late receipt ACK is
  fired from `loop()`.

Everything that interprets those bytes runs in `loop()`: `nibe_wire` parses frames and
verifies checksums, `nibe_queries` decides what to say, `nibe_codec` decodes values and
`nibe_screen` holds what is shown. All of it is plain C++; `nibe_codec` is built without
ESPHome, so `make test` exercises it directly.

Why ingest does not move out too: the earlier attempt failed on driver and scheduling
latency at *ingest*, not on parsing. A stalled task can still cost us a reply — the
quiet-line guard drops it rather than letting it land in the next exchange, and the
master falls back to its absent-node poll — but it cannot desync the wire.

**IRAM discipline is a build check, not a convention.** A flash-resident callee in the
ISR faults with a cache error while OTA disables the flash cache (observed live: the
device rebooted mid-upload and then refused every retry). The ISR surface is an explicit
list in `tools/iram_audit.py`; `make gate` fails if a listed symbol lands in flash.

## Modes: the sniffer always runs, the emulator is a switch

Capture and decode are independent of whether we transmit: every build records every
exchange. The `role:` key records the build's intent, and it is one of three factors,
not a gate on its own:

- `role: emulator` — the build intends to impersonate the panel (config intent);
- `rs485_bus: active: true` — the transport may drive the line (config capability);
- the arm switch — the operator's permission (`armed`);
- `emulating()` = intent AND transmit-capable AND armed.

`role: emulator` on a passive transport is refused loudly at setup and stays inert.
The sniffer config (`role: passive`, `active: false`) cannot transmit at all:
`send_burst` refuses at the transport, independently of the arm switch.

Host builds feed the same engine through a `tcp_uart` stream (stream mode), so
`make gate` exercises the real parser and query code against `sim/vvm300_sim.py`,
asserted byte-exact; only byte capture differs.

## Behaviour layer (as built)

The panel is a listener that answers. Everything it does is one of three verbs:
sniff never transmits; respond and request speak only through the transport's single
gate onto the wire.

| verb | when | where | transmits |
|---|---|---|---|
| **sniff** | every byte, both directions, always | transport capture → `ExchangeParser` | never |
| **respond** | inside our own slot, or to a master question | interrupt-side tracker/emitter; answers built in `NibeBus::on_frame` | yes |
| **request** | when the panel has something to say | `nibe_queries` → outbound queue | yes, on our next granted poll |

The split is structural: `ExchangeParser` reports through `ExchangeSink` and has no
transport pointer; queries send through `QueryContext` and never see a byte;
`send_burst` is the transport's single gate onto the wire. "Decode cannot touch the
wire" and "a sniffer cannot transmit" hold by construction.

Obligatory wire mechanics belong to the responder, whoever asked: the poll reply, the
receipt ACK after a push, the closing ETX of our own frame and echo rejection are
timing, not intent.

### Units

| unit | owns | file |
|---|---|---|
| transport | bytes, gaps, bursts, EN, echo mute | `rs485_bus` |
| slot tracker | poll pair, our slot, grant, close, frame end | `nibe_bus` (ISR) |
| `ExchangeParser` | boundaries, checksum, len-lie rescue, payload | `nibe_wire` |
| `QueryScheduler` + `Query` | repeat refill, poll-reply policy, outbound commands | `nibe_queries` |
| `nibe_codec` | value decode/encode, passive 0x72 pairing | `nibe_codec` |
| `ScreenState` | rows, symbol strip, knob, content latch | `nibe_screen` |

Queries and answers are different things: an outbound command is a unit (ADR-0003);
answers to master questions — the 0x62 menu reply and the 0xD4 clock — belong to the
responder, because the master asked.

### A repeating request is a predicate

The content request repeats while `ScreenState::any()` is false and the queue is still
below half; keys, knob events and register reads are once-off. The scheduler tops up
repeatables only while no once-off frame is waiting, and the poll reply is `ENQ` iff
something is deliverable, else the 2/2 burst/breath governor. That governor is emulator
policy, not a wire fact ([hypotheses.md](hypotheses.md) 1).

Deliberately **not** built, because the wire has not asked for it: per-query backoff,
queue priority for key presses (the queue is FIFO, and the pump pauses while a once-off
frame waits), and answer correlation beyond the passive 0x72 pairing.

## Safety properties (lessons from the failed earlier attempt)

1. Structured parsing: our replies fire only from slot-tracker positions reachable
   through exchange boundaries — payload bytes (`00 F9`-lookalikes) can never mimic
   a poll. On hardware a marked byte is a boundary token, which also resyncs the
   tracker after a length lie.
2. Never retry: a refused or lost burst is counted and dropped; no valve that
   re-arms transmission after anomalies.
3. On bad frame checksums: silence, then hunt for the next ETX/gap boundary.
4. `enq_pending_` survives gaps (masters grant slowly); any poll pair revokes it —
   the grant for another node's ENQ can never trigger our delivery.
5. Key frames are dropped if the master stops polling us for `no_poll_timeout`
   (default 30 s) so stale button presses can't dump into a restarted bus.
6. The ISR path is audited: `make gate` fails if any ISR-reachable symbol leaves
   IRAM, because a flash-resident callee faults while OTA writes disable the cache.
