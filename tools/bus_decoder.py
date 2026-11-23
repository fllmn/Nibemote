#!/usr/bin/env python3
"""Bus decoder: what does each node answer, per command?

Ingests sniffer/monitor captures (any era: log lines of the form
`frame cmd=XX sender=XXXX len=N data=.. ..`) and produces:

  1. inventory — per (sender, cmd): frame count, lengths, how many distinct
     payloads, byte-position volatility (which bytes are static vs live),
     sample payloads. Known decodes from docs/commands.md are annotated
     (display rows rendered as text, F2025 sensor words as LE s16).
  2. sensor timeline — F2025 word blocks (cmd 0x65) decoded over time.
  3. correlation — the display's row 2 (Varmvattentemperatur) as a number,
     joined against the temporally closest F2025 word block, so eyeballing
     which register tracks the displayed value is immediate.

Usage:
    tools/bus_decoder.py captures/live-7.txt [captures/live-9.txt ...]

This is a passive/offline tool (repos's safety rules: analysis only).
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field

LINE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d\.\d\d\d)\]\[[DIEWC]\]\[nibe:\d+\]:\s+"
    r"frame cmd=([0-9A-Fa-f]{2}) sender=([0-9A-Fa-f]{4}) len=(\d+) data=([0-9A-Fa-f ]*)"
)

# docs/commands.md — the commands we can already name
CMD_NOTES = {
    0x50: "row1: binary symbol strip (3 bytes)",
    0x51: "row2: latin-1 text (temp value)",
    0x52: "row3: latin-1 text (label)",
    0x53: "row4: latin-1 text (menu + clock)",
    0x55: "relay board push (00F5)",
    0x62: "menu request (-> reply 0F)",
    0x63: "menu-ish (0F seen)",
    0x65: "F2025 sensor block: LE s16 pairs, value/10 = unit ('C read)",
    0x71: "F2025 status request/answer pair",
    0x72: "F2025 pair (req `05 11` -> `.. 00`)",
    0xD4: "clock query/report: BCD yy mm dd hh mm ss + tail",
    0xFF: "keepalive (empty)",
    0xA1: "F1 accessory answer (len 1)",
    0xC4: "parameter-ish (len 0 push)",
    0x40: "panel keys/requests (key, knob, content request)",
}

NODE_NOTES = {
    0x000A: "VVM300 CPU (master)",
    0x00F9: "display card (panel / us)",
    0x00F5: "relay board",
    0x00F1: "accessory (polled)",
    0x41C9: "F2025 heat pump",
}

ROW_CMDS = {0x50, 0x51, 0x52, 0x53}


@dataclass
class Frame:
    t_seconds: float  # wall clock within the capture (monotonic per file)
    cmd: int
    sender: int
    data: bytes


@dataclass
class Group:
    frames: list = field(default_factory=list)

    def lengths(self) -> set:
        return {len(f.data) for f in self.frames}

    def volatility(self) -> list | None:
        """per byte position: fraction of frames where the byte changed vs the
        first sample — None for ragged groups (mixed lengths)"""
        lens = self.lengths()
        if len(lens) != 1:
            return None
        n = lens.pop()
        if n == 0 or len(self.frames) < 2:
            return None
        first = None
        changed = [0] * n
        for f in self.frames:
            if first is None:
                first = f.data
                continue
            for i in range(n):
                if f.data[i] != first[i]:
                    changed[i] += 1
        return [c / (len(self.frames) - 1) for c in changed]

    def distinct(self, cap=8) -> list:
        seen = {}
        for f in self.frames:
            seen.setdefault(f.data, 0)
            seen[f.data] += 1
        return sorted(seen.items(), key=lambda kv: -kv[1])[:cap]


def parse(path: str) -> list[Frame]:
    frames = []
    prev_t = None
    day_offset = 0
    with open(path, errors="replace") as fh:
        for line in fh:
            m = LINE_RE.match(line.lstrip())
            if not m:
                continue
            hh, mm, ss = int(m.group(1)), int(m.group(2)), float(m.group(3))
            t = hh * 3600 + mm * 60 + ss
            if prev_t is not None and t < prev_t - 60:  # wallclock rolled past midnight
                day_offset += 86400
            prev_t = t
            frames.append(Frame(t + day_offset, int(m.group(4), 16), int(m.group(5), 16),
                                bytes.fromhex(m.group(7).strip() or "")))
    return frames


def latin1(data: bytes) -> str:
    try:
        s = data.split(b"\x00")[0].decode("latin-1")
        return "".join(ch if ord(ch) >= 32 else "?" for ch in s)
    except UnicodeDecodeError:
        return repr(data)


def s16_words(data: bytes) -> list:
    out = []
    for i in range(0, len(data) - 1, 2):
        out.append(int.from_bytes(data[i : i + 2], "little", signed=True))
    return out


def fmt_hms(t: float) -> str:
    t = t % 86400
    h = int(t // 3600)
    m = int((t % 3600) // 60)
    s = t % 60
    return f"{h:02d}:{m:02d}:{s:04.1f}"


def inventory(frames: list) -> None:
    groups = defaultdict(list)
    for f in frames:
        groups[(f.sender, f.cmd)].append(f)

    by_node = defaultdict(list)
    for (sender, cmd), fs in groups.items():
        by_node[sender].append((cmd, fs))

    print("=" * 78)
    print("INVENTORY — every node x command observed, static/live bytes, samples")
    print("=" * 78)
    for sender in sorted(by_node):
        note = NODE_NOTES.get(sender, "?")
        print(f"\n## node {sender:04X} — {note}")
        for cmd, fs in sorted(by_node[sender], key=lambda c: -len(c[1])):
            g = Group(fs)
            note = CMD_NOTES.get(cmd, "")
            lens = ",".join(str(l) for l in sorted(g.lengths()))
            uniq = len({f.data for f in fs})
            vol = g.volatility()
            vol_s = ""
            if vol is not None and len(vol) > 0 and len(fs) > 1:
                marks = "".join(
                    "." if v == 0 else ("s" if v < 0.05 else ("S" if v < 0.5 else "L"))
                    for v in vol
                )
                vol_s = f"  bytewise[{marks}]\n" \
                        f"    (. static, s sparse <5%, S some <50%, L live >=50%)"
            print(f"  cmd 0x{cmd:02X}  n={len(fs):<6} len={lens:<10} uniq={uniq:<5} {note}")
            if vol_s:
                print(vol_s, end="")
            for sample, count in g.distinct(4):
                extra = ""
                if cmd == 0x65:
                    extra = f"   -> s16: {[w for w in s16_words(sample)]}"
                elif cmd in ROW_CMDS and len(sample) > 0:
                    extra = f'   -> "{latin1(sample)}"'
                elif cmd == 0xD4 and len(sample) == 7:
                    b = lambda i: (sample[i] >> 4) * 10 + (sample[i] & 0xF)
                    extra = f"   -> 20{b(0):02d}-{b(1):02d}-{b(2):02d} {b(3):02d}:{b(4):02d}:{b(5):02d} tail={sample[6]:02X}"
                if count < len(fs):
                    shown = f"x{count}"
                else:
                    shown = ""
                print(f"      {sample.hex(' ')!s:<50} {shown}{extra}")


def correlate(frames: list, window: float = 8.0) -> None:
    """row 2 (the display's primary value, e.g. HW temp '  48.2°C') against the
    temporally closest F2025 sensor block (cmd 0x65 from 41C9)"""
    rows = []
    sensors = []
    for f in frames:
        if f.sender == 0x000A and f.cmd == 0x51 and len(f.data) >= 5:
            text = latin1(f.data)
            m = re.search(r"(\d+)\.(\d)", text)
            if m:
                rows.append((f.t_seconds, float(f"{m.group(1)}.{m.group(2)}"), text.strip()))
        elif f.sender == 0x41C9 and f.cmd == 0x65 and len(f.data) >= 10:
            sensors.append((f.t_seconds, s16_words(f.data)))
    if not rows or not sensors:
        print("\n(no row-2 frames or F2025 sensor blocks — nothing to correlate)")
        return

    print("\n" + "=" * 78)
    print("CORRELATION — display row 2 value  vs  nearest F2025 0x65 words (<= +%ds)" % window)
    print("=" * 78)
    print(f"{'time':>10}  {'row2':>7}  word_idx: value/idx of the F2025 block")
    for rt, val, text in rows:
        best = min(sensors, key=lambda s: abs(s[0] - rt))
        if abs(best[0] - rt) > window:
            continue
        words = best[1]
        cands = " ".join(f"[{i}]={w}" for i, w in enumerate(words))
        print(f"{fmt_hms(rt):>10}  {val:6.1f}°  {cands}")
    print("\nhint: the register that tracks row2's value (also check /10 and "
          "sign; docs say words are value/10 in 'C for the F2025 block)")


def main(argv):
    files = [a for a in argv[1:] if not a.startswith("-")]
    if not files:
        print(__doc__)
        return 2
    all_frames = []
    for path in files:
        n = len(all_frames)
        all_frames.extend(parse(path))
        print(f"# parsed {path}: +{len(all_frames) - n} frames", file=sys.stderr)
    print(f"# total frames: {len(all_frames)}")
    inventory(all_frames)
    correlate(all_frames)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
