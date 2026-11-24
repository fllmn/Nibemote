# Open hypotheses & unexplained observations

Everything the wire has shown us but we cannot yet call fact. Entries are
ordered by how much they matter to the emulator. Each carries the evidence,
what speaks against it, and the capture or experiment that would settle it.
When an item graduates to fact it moves to
[protocol.md](protocol.md) (wire behavior) or [commands.md](commands.md)
(commands and registers); when one dies, leave the rejection note here so
nobody re-imports it.

## 1. What gates the master's row push — the panel's request rate, or the master's own schedule?

The request *rhythm* the emulator ships (2 ENQ polls, then 2 bare-ACK polls, repeating)
was invented in this repo. It has no capture behind it, no note in the prior
../nibemote, and `sim/vvm300_sim.py` both asserts it and documents it, so the sim
cannot detect its removal. Before that rhythm is designed into anything, we need to
know whether the master cares.

Device capture of the startup phase (emulator, `log_level: 1`, arm switch on):

- 4 request deliveries `40 67 00 00` (ENQ -> frame -> receipt `06` -> close `03`),
  in two bursts of 2, `21:31:46.413`..`.578`
- two bare ACKs at `.445`/`.464` **inside** the request phase — nothing pushed into them
- bare ACK at `.579`, then the master's `0x51` (row 2) 18 ms later; the same shape
  repeats for `0x53` (`.593`) and `0x52` (`.651`), each 13-17 ms after a bare ACK
- the frame/ETX/`03` and receipt-`06` timings match protocol.md exactly

Confirmed by it: the master pushes rows into the panel's own slot (reply byte ->
master frame -> panel receipt `06` -> master close `03`), and it does so at a moment
of its own choosing.

- For "our request rate is not the gate": the master had two free bare-ACK slots
  mid-request-phase and left them unused, then pushed after the 4th request. Its
  readiness tracked its own schedule, not our slots.
- For "a specific count/rhythm is required": those 4 requests are the only sample.
  One sample cannot separate "4 requests are needed" from "the master became ready
  ~120 ms after hearing us", and cannot say whether a push follows an ENQ reply at
  all (no capture shows one; `push_after_enq()` in the sim assumes it does).
- Settle by: vary the announce bound in the task-side scheduler — queue N request
  frames, then answer bare ACK for a back-off window, N ∈ {1,2,4,8} — and confirm rows
  `0x51`-`0x53` arrive for every N. One session of continuous requesting (no back-off)
  also answers whether the master pushes into a slot whose reply was ENQ.
- Shipped today: `ANNOUNCE_BURST`/`ANNOUNCE_BREATH` (nibe_queries.h) drive the 2/2
  rhythm from the task-side scheduler; the pump refills to half the queue. If the
  counters turn out to be unnecessary, the reply byte reduces to "ENQ iff a deliverable
  frame is queued" and the announce phase becomes pure task-side policy with no
  governor.

## 2. The knob event: `[03, placement:u8, 00, 00]` on cmd 0x40 — OWNER RECALL

The physical knob (VÄRMEKURVA FÖRSKJUTNING — heating curve offset) is believed
to ride the same cmd-0x40 event family as the keys, with a knob-specific id
**0x03** (the one never-observed value in the 01..04 key range) and the knob's
absolute **placement as a u8 in data[1]** — where requests/keys carry the
relayed system value.

- For: id 03 is conspicuously missing in otherwise-contiguous key ids; the
  panel must relay the knob mechanically (it is a front-panel input); the
  data[1] slot demonstrably means different things per data[0].
- Against: NO knob rotation was ever captured — the user pressed plus/minus/
  enter during every phase-1/2 capture, never turned the knob. The id and the
  placement coding could both be wrong.
- Settle by: reattach the original display, run the sniffer
  (`configs/nibemote-sniffer.yaml`), turn the knob slowly across its full
  travel and back, one detent at a time, recording the frames. The frames
  that appear during rotation answer id, direction coding, placement scale,
  and detent count in one pass.
- Shipped today: the `Display knob` number and the :8080 dial emit
  `[03, placement, 00, 00]` per change (placement 127 default, ±12/detent).
  If a capture contradicts, only constants in `nibe_frame.h` /
  `nibe_web.cpp` change.

## 3. cmd-0x40 data[1] = outdoor-temperature-shaped relayed system value

The second data byte of the panel's request AND key frames is not a flag: it
drifted 0x85 -> 0x88 -> 0x89 across evening captures and 0x67 -> 0x68 the next
morning — the same value relayed in both frame types. The old ../nibemote
project's spec labeled it a fixed 0x83 flag; that is disproven.

- For: F2025 0x65 telemetry samples carried 67/68-valued fields on the morning
  capture; the values look like tenths (8.5/8.8/8.9 vs 6.7/6.8).
- Against: live-7's F2025 w0 (outdoor at the pump) read 4.6-5.2 while the panel
  relayed 0x88 (8.8) the same evening — similar shape, not the same source.
  Could be a different sensor (outdoor at the indoor unit), a filtered value,
  or something else entirely.
- Settle by: one multi-hour passive capture correlating the byte against every
  F2025 field (w0 first suspect), or menu navigation on the physical display to
  find which screen shows 8.8/6.7.
- Engine note: `request_byte` defaults 0x88 (the live-9-era value); the
  current config pins 0x67. Long-term the emulator should relay what the
  master most recently sent, if this capture shows the master ever pushes it
  — no evidence yet that it does.

## 4. 0x71 status block, bytes 3-4 = "compressor starts so far"

The 16-byte status block is byte-packed; bytes 7-8 (compressor operating
hours) are display-verified. Bytes 3-4 wander: 40034 -> 40182 over the
live-7 -> Nov-15 window (+148 in ~4.6 days ≈ 1.35/h — plausible cycling for a
November heat load), while all other block cells held constant across weeks.

- For: plausible rate; sits adjacent to the hours field; counts monotonically.
- Against: never display-matched.
- Settle by: read "ANTAL STARTER KOMPRESSOR" on the F2025 service menu and
  compare with the raw u16 at offset 3 (published as the "compressor starts
  (candidate)" sensor).

## 5. Unmapped F2025 fields

- 0x65 words 5 (constant 16384), 6 (large non-thermal blob), 7 (constant
  -32767). Constants suggest flags/scale metadata; w6 could be a bitfield or
  packed counters. Settle by menu walk on the machine comparing readouts to the
  published hex sensors.
- 0x71 bytes 0-2, 5-6, 9-15: several held perfectly constant across two weeks
  (configuration or capacity data); the rest unexamined. The block is
  byte-packed — new fields should be probed at BYTE offsets via the generic
  `f2025_register` sensor.

## 6. cmd 0x50 row-1 symbol strip bits

Row 1 is a 3-byte bitmap of front-panel symbols. The virtual display renders
it with a heuristic bit table (see `nibe_web.cpp`, derived by correlating
captures with what the panel shows) — six symbols annotated, none of it
machine-confirmed. A menu-driven capture session flipping each statescreen
(heating, hot water, alarm...) would fix the table.

Observed (2026-11-24, web page during a defrost cycle): **`b2.0` is the defrost
state**. The earlier guesses on `b2.2` ("defrost?") and `b2.0` ("addition
heater?") were wrong. Addition heater I/II/III remain unmapped.

## 7. Node 0x00FA — the boot-scan slot (investigation parked)

00FA was polled 14x in BOTH boot captures and never in steady state — paired
temporally with the display's first content request. All probing was parked at
the user's call (2026-11-14), with one question left unanswered:

- Fa-probe-1: ENQing as 00FA changed the master's scan cadence (~2 ms re-polls)
  but earned **zero grants** — payload was `40 88` (not the session's 0x67).
  Unverified whether the boot scan grants a byte-faithful answer.
- probe-fa-2 proved the scan is strictly master-boot behavior: a mid-run
  display vanish/reannounce never re-triggered it.
- Settle by: cold-boot the master with a sniffing ESP answering nothing and
  watch what the master would have pushed to 00FA, or park this for good —
  the emulator works without it.

## 8. The `05 <reg>` register space (0x72 read / 0x76 write)

Master reads/writes `05 <reg>` on the display, relay AND F2025 slots with
different per-node values (0x40/0x41/0x42 on display+relay; 0x11/0x28/0x1F on
F2025). Looks like a bus-wide register mirror with per-node cells. Nothing
about the register semantics or the panel's values is decoded. A capture
session changing one VVM300 setting at a time would map registers to menu
parameters — the clean path to a future parameter-write feature.

Observed on the wire: register traffic is **master-initiated**. The master asks
`72 05 <reg>` (len 2) and the addressed node answers in the same window by ENQing:

- the display answers `72 05 <reg> <val>` (len 3) — `0x40=06, 0x41=00, 0x42=00`;
- the F2025 answers a single value byte (len 1) — register `0x11` read
  `00/03/04/11/14` across captures.

No node was seen initiating a read or a write. Tested on the device (2026-11-24):
the panel sent `72 05 11` and `72 05 40`; the master receipt-ACKed both and sent
nothing else — no answer frame, NAK or ETX. The master does not serve register
reads, so the panel cannot read the master's registers.

Consequences: the values are obtainable by capturing the master's own reads (the
passive pairing attributes each answer to the register the master asked the polled
node for). At startup the master sweeps the panel's block `0x40`-`0xBF` (128 reads,
one per exchange); the real display answers every one and the values are constant
across captures (see commands.md). nibemote now answers that sweep from the captured
table, in-window: ENQ in the receipt slot, the frame on the grant. Its own probe
requests remain inert.

## 9. Idle-slot keepalive 0xFF purpose

Shape is verified (empty push, ~11/s in idle, receipt-ACKed like all pushes)
but the *purpose* is unproven — liveness ping, display-side state poke, or
frame-budget filler. Harmless to receipt-ACK and ignore.

## 10. Relay board traffic (0x55 / 0x45)

0x55 values evolve (2-byte control/state pairs); 0x45 `01` is one of the only
two unprompted event types ever observed bus-wide. Untested whether 0x45
edges correlate with F2025 compressor starts — same-capture correlation with
the 0x65 hot-gas word would answer it in one pass.

## 11. Accessory slot 0x00F1 traffic (0xA1 etc.)

0xA1 one-byte payloads increment like a percentage or step setting, pushed
~90x in the steady-state window, never answered. No hypothesis beyond
"control/status". Correlate against telemetry for a first guess.

## 12. One-shot commands (0x32/0x33/0x35/0x63, 0xC4/0xC5, 0xEE, 0x22, 0xAE/0xAF/0xA3)

Observed shapes only. 0x22 to the F2025 is verified to be the master's
boot-time link check *by context* (not content); 0xEE draws an empty query and
`00 78` answer on the F2025 slot; the rest are boot/menu-window one-shots.
Passive captures around deliberate state changes (hot water tap, thermostat
bump) would attribute them.

The panel can also *send* 0x32 (empty) as a query — the "Query master state
(0x32)" button. Only the master->panel direction has been observed, so the
master's answer, ETX or silence is the experiment; the frame and tx logs show it.

**Result (device, 2026-11-24):** the panel sent `32 00 F9 01 00 CA`; the master
receipt-ACKed it and sent nothing else — no answer frame, NAK or ETX. That fits
the master->panel captures: the master pushes `32 00` about every 60 s and the
real panel answers with its receipt ACK only. So 0x32 is a one-way notification in
the master->panel direction, and the panel-sent direction has no observable
answer. It is not a query channel to the master.

## 13. REJECTED: the forum 0xC0 parameter map (do not re-import)

The Elektronikforumet thread t=13714 ("PC-styrning av Bergvärmepump",
via ../nibemote) documents cmd-0xC0 parameter blocks with an index map
(0x0A curve slope, 0x0B curve offset, 0x26 RCU offset...). That map is for a
**different product line** (360P-era) whose wire format is NOT this VVM300's
bus: our master never sent 0xC0 to the display in any capture, and the knob's
believed format here is the cmd-0x40 event family (item 2). The knob number
was briefly implemented as an 0xC0/0x0B write and reverted (commit 4ba0591).
If a capture ever shows a real 0xC0 exchange, treat it as NEW evidence.

## 14. The panel clock's +4.9 h offset (cause)

Verified fact: the panel's clock ran +4.9 h ahead of local on two consecutive
days (02:22 vs 21:27 real; 01:12 vs 20:17 real). The likely cause is simply an
uninitialized/stale RTC never synced by its owner — the machine predates the
emulator. No test worth running; with `d4_reply` enabled the esp provides the
clock instead.

## 15. The wire char carries a 9th bit (address-marked, MDB style) — CONFIRMED

Origin: the Nibe 1130 uses a hardware 9-bit UART at 19200 (MatsJo/
nibe-analytics sketch); the Nibe 360P doc (MackanT/NibeVVP, same forum thread
as item 13) describes exactly this scheme with MARK/SPACE parity and node
addresses whose low bytes match our bus (F5 relay, F2025 display).

CONFIRMED on the VVM300/F2025 wire by the 8E1-slot probe (historically a
sniffer switch; the slot is now a permanent line-format fact, so the probe
census was deleted — docs/adr/0004): per byte,
bit9 = parity-error XOR data parity. Census over two windows, live traffic:

| byte class                    | 9th slot | count   |
|-------------------------------|----------|---------|
| poll pairs (master -> any)    | 1        | 252/252 |
| frame bytes, both directions  | 0        | 593/593 |
| poll-reply ACK 06 (slaves)    | 0        | all     |
| ENQ 05 (F2025 answers a push) | 1        | 7/7     |
| grants, receipt ACKs 06      | 0        | all     |
| ETX 03, master's own push    | 1        | 4/4     |
| ETX 03, F2025's own frame    | 0        | all     |

Independent corroborations: 8N1 mode counts one framing error per data byte
(bit9=0 sits in the stop slot, does not cascade — why weeks of clean captures
never showed it); the min byte-to-byte delta measured 560-608 us, consistent
with 11-bit chars, not 8N1's 521.

It does NOT explain the reply-timing/pacing work: our TX is 8N1 (stop bit = 1
in every byte), so every byte we send reads bit9=1 to a marked listener — and
the master digests our frames anyway. The mute-turnaround model (protocol.md)
stands regardless.

The mark looks like MPCM wake-up addressing: polls and the ENQ token are
marked so sleeping UARTs engage; data moves unmarked inside an open exchange.

Our own TX now speaks the same shape: `nine_bit` (rs485_bus config, default
on) emits each byte's 9th slot via a per-byte EVEN/ODD parity flip — the
display role marks only its ENQ; ACKs, frames and our closing ETX go out
unmarked, exactly the census table above. Ours previously read marked on every
byte (8N1 stop lands in the slot) and the master digested it anyway, so
this is citizen-caching toward the other nodes, not a fix for a failure —
VALIDATED LIVE (2026-11-23): the emulator ran nine_bit against the VVM300
and the master granted the (now truly marked) ENQ, digested all frames
and served the rows exactly as with 8N1. Remaining open sub-items: the
wake/ignore table (does a node drop unmarked bytes outside its slot?),
and the exact stop/char shape behind the per-byte framing errors that
persist in both 8N1 and probe modes (needs a per-edge dump, e.g. an RMT
capture).

## Process

Doing any of these: `configs/nibemote-sniffer.yaml` for pure captures; the
arm switch + `log_level: 2` for µs-grade ledgers of emulator behavior;
`sensors/` `f2025_register` stanzas to probe new fields without reflashing
logic. Captures live in `captures/` (`live-N`, `probe-fa-N`, `hotswap-N` are
the load-bearing ones for the claims in protocol.md).
