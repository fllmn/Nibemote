# Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ ESPHome entity layer                                                │
│   text_sensor (rows, last key)  sensor (counters, gap)  button      │
│   binary_sensor (polled) — plain HA entities via the hub's sinks     │
├─────────────────────────────────────────────────────────────────────┤
│ components/nibe   — Nibe feature layer (task + ISR halves)          │
│   process_event_() loop side: logging, row/key decode, announce       │
│   trigger, safety watchdogs (no-poll timeout), entity dispatch      │
│   ────────────── ISR side ───────────────                            │
│   NibeBus byte FSM: poll parse / reply decision (ACK vs ENQ),        │
│   frame codec + XOR-8, receipt-ACKs, frame+ETX delivery from the     │
│   outbound queue, gap-anchored recovery, ETX-hunt desync mode        │
├─────────────────────────────────────────────────────────────────────┤
│ components/rs485_bus — generic RS485 transport (protocol-blind)      │
│   ESP32-S3 ISR: per-byte RX interrupts, hw rx-timeout gap events,    │
│   EN assert/drop around bursts (TX_DONE), hw RS485 echo mute,       │
│   raw byte trace ring (ISR→loop), stats                              │
│   Stream mode: same engine hooks fed from an esphome `uart`         │
│   (host builds / sim) — identical engine code either way            │
├─────────────────────────────────────────────────────────────────────┤
│ UART1 @ 19200 8N1, GPIO17 TX / GPIO18 RX / GPIO21 EN (Waveshare)     │
└─────────────────────────────────────────────────────────────────────┘
```

## Why the engine lives in the ISR

The bus master expects display replies within ~1–2 ms of the poll, and the previous
attempt showed that task-context handling (IDF driver latencies, loop scheduling
under WiFi, echo confusion) desyncs the parser and leaks bytes into other nodes'
exchanges — corrupting the VVM300↔F2025 telemetry is what stopped the heating.
Nibemote's engine therefore runs entirely inside the UART ISR:

- **RX**: `rxfifo_full` threshold 1 → every byte is consumed and time-stamped in the
  ISR; the poll-reply decision and frame parsing happen inline. Replies are on the
  wire microseconds after the poll's low byte lands.
- **Gaps**: hardware `rx-tout` (4 byte-times ≈ 2.1 ms) → a gap event re-anchors the
  FSM to a boundary. Byte times are too fine for loop-context timing to fake.
- **TX**: `send_burst()` asserts EN (GPIO21), fills the TX FIFO, arms the `tx_done`
  interrupt; the ISR drops EN on `tx_done` — the same instant the IDF RS485
  half-duplex driver releases DE. Bursts are limited to 96 bytes (FIFO headroom).
- **Echo**: hardware RS485 app-ctrl mode (`rs485rxby_tx_en`) mutes the receiver
  while transmitting, so our own bursts cannot enter the FSM. A narrow stateful
  echo discriminator remains as belt-and-braces.
- **IRAM discipline (learned the hard way, live)**: ISR + its callees — including
  static helpers like the ring/queue atomics — are IRAM_ATTR; everything they touch
  is DRAM. Any flash-mapped byte in the interrupt path faults with a Cache error as
  soon as a boot-time flash write overlaps a bus interrupt (observed as OTA boot
  rollbacks). The interrupt is
  allocated with `ESP_INTR_FLAG_IRAM` so the bus keeps being answered even while
  flash cache is disabled during OTA writes. `esp_timer_get_time()` is IRAM-safe.

Everything human-facing is asynchronous: the ISR pushes compact events (polls,
control bytes, frames with a payload arena copy, gaps, anomalies) into a DRAM ring
that the loop drains for logs, counters and entities.

## Passive vs emulator

The same engine runs in both builds:

- `role: passive` + `rs485_bus: active: false` (the sniffer): parsing and recording
  only, EN hard low, `send_burst()` refuses on hardware — nothing can be transmitted.
- `role: emulator` + `active: true` (the panel replacement): additionally answers
  polls, receipt-ACKs master frames, announces after the 0x22 ping, replies to 0x62,
  and delivers key frames queued by the Home Assistant buttons.
- The nibe hub downgrades itself to passive with a loud error if someone configures
  `role: emulator` without `active: true`.

Host builds (configs/nibemote-host.yaml + sim/vvm300_sim.py) exercise exactly the
same engine through a `tcp_uart` in stream mode — the full exchange suite
(announce handshake, menu reply, rows, keepalives, key injection, corrupted-frame
silence) is asserted byte-exact and must pass before any hardware flash.

## Safety properties (lessons from the failed earlier attempt)

1. Structured parsing: our replies fire only from poll-parse positions reachable
   through exchange boundaries — payload bytes (`00 F9`-lookalikes) can never mimic
   a poll through the FSM.
2. Never retry: a refused or lost burst is counted and dropped; no valve that
   re-arms transmission after anomalies.
3. On bad frame checksums: silence, then hunt for the next ETX/gap boundary.
4. `enq_pending_` survives gaps (masters grant slowly); any poll pair revokes it —
   the grant for another node's ENQ can never trigger our delivery.
5. Key frames are dropped if the master stops polling us for `no_poll_timeout`
   (default 30 s) so stale button presses can't dump into a restarted bus.
