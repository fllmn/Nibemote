#!/usr/bin/env python3
"""ETX-attribution analyzer: who sent the closing 03 after a receipt 06?

live-9.txt carries wire-true µs event stamps (@ts (+delta) tails). For every
frame -> ctrl 06 -> ctrl 03 chain we record, classified by the frame's sender:

  000A (master push; the 06 is the PANEL's receipt)  -> the 03 must be the
       master's (a receiver never closes foreign frames): master fingerprint
  00F9 (granted display frame; the 06 is the MASTER's receipt) -> the 03 is
       the DISPUTED byte: master burst would be ~1 byte time (~520 us), a
       panel-sent close would show the panel's turnaround instead
  41C9 (granted F2025 frame) -> same question for the F2025 slot

Reference fingerprints collected alongside:
  delta(poll -> panel ENQ)        the panel's RX->TX turnaround
  delta(panel ENQ -> master grant) the master's turnaround
  delta(frame -> receipt 06)       the receiver's receipt turnaround, per sender
  delta(bare 06 -> 03)             the master's measured idle-slot close
"""

from __future__ import annotations

import re
import statistics
import sys

PAT = re.compile(
    r"^\[(\d+:\d+:\d+\.\d+)\]\[[DIWC]\]\[nibe:(\d+)\]: (.*?)(?:\s+@(\d+) \(\+(\d+)\))?\s*$"
)
BYTE_US = 1_000_000 / 19200 * 10  # 520.8 us char time at 19200 8N1


class Stat:
    def __init__(self, name: str) -> None:
        self.name = name
        self.vals: list[int] = []

    def add(self, v: int) -> None:
        self.vals.append(v)

    def report(self) -> str:
        if not self.vals:
            return f"{self.name:34s} (n=0)"
        vs = sorted(self.vals)
        med = statistics.median(vs)
        p90 = vs[min(len(vs) - 1, int(len(vs) * 0.9))]
        return (
            f"{self.name:34s} n={len(vs):4d}  min={vs[0]:6d}  med={med:7.0f}  "
            f"p90={p90:6d}  max={vs[-1]:6d} us"
        )


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "captures/live-9.txt"
    stats = {
        "poll->ENQ (panel turnaround)": Stat("poll->ENQ (panel turnaround)"),
        "ENQ->grant (master turnaround)": Stat("ENQ->grant (master turnaround)"),
        "idle 06->03 (master idle close)": Stat("idle 06->03 (master idle close)"),
        "push: frame->06 (panel receipt)": Stat("push: frame->06 (panel receipt)"),
        "push: 06->03 (MASTER close, known)": Stat("push: 06->03 (MASTER close, known)"),
        "disp: frame->06 (master receipt)": Stat("disp: frame->06 (master receipt)"),
        "disp: 06->03 (DISPUTED)": Stat("disp: 06->03 (DISPUTED)"),
        "f2025: frame->06 (master receipt)": Stat("f2025: frame->06 (master receipt)"),
        "f2025: 06->03 (disputed)": Stat("f2025: 06->03 (disputed)"),
    }

    last_kind = None  # ("poll"|"enq"|"ack6"|"frame", ts, sender)
    in_frame_gap = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PAT.match(line.rstrip("\n"))
            if not m:
                continue
            _, _, ev, ts_s, d_s = m.groups()
            if ts_s is None:
                continue
            ts = int(ts_s)
            delta = int(d_s) if d_s else 0

            if ev.startswith("poll 00F9"):
                last_kind = ("poll", ts, None)
            elif ev.startswith("ctrl 05"):
                if last_kind and last_kind[0] == "poll":
                    stats["poll->ENQ (panel turnaround)"].add(delta)
                last_kind = ("enq", ts, None)
            elif ev.startswith("ctrl 06"):
                if last_kind and last_kind[0] == "enq":
                    stats["ENQ->grant (master turnaround)"].add(delta)
                elif last_kind and last_kind[0] == "frame":
                    key = f"{last_kind[2]}: frame->06 (master receipt)"
                    if last_kind[2] == "push":
                        stats["push: frame->06 (panel receipt)"].add(delta)
                    elif last_kind[2] == "disp":
                        stats["disp: frame->06 (master receipt)"].add(delta)
                    elif last_kind[2] == "f2025":
                        stats["f2025: frame->06 (master receipt)"].add(delta)
                elif last_kind and last_kind[0] == "ack6_bare":
                    pass
                if last_kind and last_kind[0] == "frame":
                    # receipt 06 after a frame: remember the frame sender class
                    last_kind = ("receipt", ts, last_kind[2])
                else:
                    last_kind = ("ack6_bare", ts, None)
            elif ev.startswith("ctrl 03"):
                if last_kind and last_kind[0] == "receipt":
                    if last_kind[2] == "push":
                        stats["push: 06->03 (MASTER close, known)"].add(delta)
                    elif last_kind[2] == "disp":
                        stats["disp: 06->03 (DISPUTED)"].add(delta)
                    elif last_kind[2] == "f2025":
                        stats["f2025: 06->03 (disputed)"].add(delta)
                elif last_kind and last_kind[0] == "ack6_bare":
                    stats["idle 06->03 (master idle close)"].add(delta)
                last_kind = None
            elif ev.startswith("frame cmd="):
                msender = re.search(r"sender=([0-9A-Fa-f]{4})", ev)
                sender = msender.group(1).upper() if msender else ""
                cls = None
                if sender == "000A":
                    cls = "push"
                elif sender == "00F9":
                    cls = "disp"
                elif sender == "41C9":
                    cls = "f2025"
                if last_kind and last_kind[0] == "poll" and cls is None:
                    pass
                last_kind = ("frame", ts, cls, sender)
            elif ev.startswith("gap") or ev.startswith("BAD frame"):
                last_kind = None

    print(f"byte time @19200 8N1: {BYTE_US:.1f} us (same-sender burst ≈ this)\n")
    for s in stats.values():
        print(s.report())

    # per-sample listing for the disputed classes, to eyeball bimodality
    print("\ndisp 06->03 raw samples:")
    for v in stats["disp: 06->03 (DISPUTED)"].vals[:40]:
        print(f"  {v:6d} us   ({v / BYTE_US:5.1f} byte-times)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
