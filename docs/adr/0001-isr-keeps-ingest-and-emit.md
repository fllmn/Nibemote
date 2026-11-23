# The ISR keeps ingest and emit; parsing and policy move to the task

The wire gives 0.8–2.4 ms to answer a poll and ~160 µs to start a granted frame, so byte
ingest with timestamps and the ninth bit, gap detection, and timed transmission stay in
the interrupt path. Everything that interprets those bytes — checksum, frame assembly,
command meaning, announce policy — moves to task context, where it is plain, testable C++
and where a new command can be added without touching the interrupt path. Minimal
exchange-boundary tracking stays in the ISR so that a stalled task cannot desync the wire.

Considered alternatives: keeping the whole FSM in the ISR (rejected: it is the reason the
engine is hard to read); moving byte ingest to a task as well (rejected: the earlier
attempt failed on driver and scheduling latency at ingest, not on parsing).
