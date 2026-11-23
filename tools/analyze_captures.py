#!/usr/bin/env python3
"""Offline analysis of nibemote2 sniffer log captures.

Feed it a saved `esphome logs` / serial console session of the sniffer build
(passed as a file or on stdin) and it reconstructs the bus picture:

  - poll schedule per node (count, min/median/max interval)
  - reply distribution per node (ACK / ENQ / silent / weird)
  - frame census by (cmd, sender): count, data lengths, checksum failures
  - key frames with names, announce/heartbeat frames, display rows as text
  - gap statistics (calibrate rs485_bus gap_symbols against them)
  - unknown commands worth investigating

Usage:
    python3 tools/analyze_captures.py captures/boot.txt
    esphome logs configs/nibemote-sniffer.yaml | tee /tmp/cap.txt
    python3 tools/analyze_captures.py /tmp/cap.txt
"""

from __future__ import annotations

import collections
import re
import statistics
import sys

# Content-anchored patterns (immune to esphome's log tag prefixes and ANSI colors)
POLL_RE = re.compile(r"(?<![0-9A-Fa-f])poll (?P<addr>[0-9A-Fa-f]{4})(?: \(own\))?")
CTRL_RE = re.compile(r"ctrl (?P<byte>[0-9A-Fa-f]{2}) \(slot (?P<slot>[0-9A-Fa-f]{4})\)")
FRAME_RE = re.compile(
    r"frame cmd=(?P<cmd>[0-9A-Fa-f]{2}) sender=(?P<sender>[0-9A-Fa-f]{4}) "
    r"len=(?P<len>\d+) data=(?P<data>(?:[0-9A-Fa-f]{2} ?)*)(?:$| )"
)
BAD_RE = re.compile(r"BAD frame cmd=(?P<cmd>[0-9A-Fa-f]{2}) sender=(?P<sender>[0-9A-Fa-f]{4})")
GAP_RE = re.compile(r"gap (?P<us>\d+) us")
KEY_RE = re.compile(r"key frame: 0x(?P<code>[0-9A-Fa-f]{2}) \((?P<name>\w+)\)")

CTRL_NAMES = {"06": "ACK", "05": "ENQ", "15": "NAK", "03": "ETX"}
KEY_LETTER = {"01": "PLUS", "02": "MINUS", "04": "ENTER"}


def ascii_or_hex(data: bytes) -> str:
    if all(32 <= b < 127 or b == 0 for b in data):
        return data.rstrip(b"\x00").decode("ascii")
    return ""


def latin1(data: bytes) -> str:
    return data.rstrip(b"\x00").decode("latin-1")


def analyze(text: str) -> int:
    polls: list[tuple[str, int]] = []
    poll_intervals: dict[str, list[int]] = collections.defaultdict(list)
    replies: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    frames: dict[tuple[str, str], list[bytes]] = collections.defaultdict(list)
    bad: collections.Counter = collections.Counter()
    gaps: list[int] = []
    keys: collections.Counter = collections.Counter()
    rows: dict[str, list[str]] = collections.defaultdict(list)

    for line in text.splitlines():
        m = POLL_RE.search(line)
        if m:
            addr = m["addr"].upper()
            polls.append((addr, 1))
            replies[addr]  # ensure node known
        m = CTRL_RE.search(line)
        if m:
            slot = m["slot"].upper()
            name = CTRL_NAMES.get(m["byte"].upper(), "?")
            replies[slot][name] += 1
        m = FRAME_RE.search(line)
        if m:
            cmd, sender = m["cmd"].upper(), m["sender"].upper()
            data = bytes.fromhex(m["data"].replace(" ", ""))[: int(m["len"])]
            frames[(cmd, sender)].append(data)
            if sender == "000A" and cmd in ("51", "52", "53"):
                rows[cmd].append(latin1(data))
            elif sender == "00F9" and cmd == "40" and len(data) >= 2 and data[1] == 0x83:
                keys.setdefault(KEY_LETTER.get(f"{data[0]:02X}", f"KEY_{data[0]:02X}"), 0)
        m = BAD_RE.search(line)
        if m:
            bad[(m["cmd"].upper(), m["sender"].upper())] += 1
        m = GAP_RE.search(line)
        if m:
            gaps.append(int(m["us"]))
        m = KEY_RE.search(line)
        if m:
            keys[m["name"]] += 1

    print("=" * 72)
    print("POLL SCHEDULE")
    print("=" * 72)
    for addr in sorted(replies):
        n = sum(replies[addr].values())
        dist = ", ".join(f"{k}={v}" for k, v in replies[addr].most_common())
        print(f"  {addr}: {n:>5} slots   {dist}")
    if not replies:
        print("  (no polls in the capture)")

    print()
    print("=" * 72)
    print("FRAMES by (cmd, sender)")
    print("=" * 72)
    for (cmd, sender), datas in sorted(frames.items()):
        lens = collections.Counter(len(d) for d in datas)
        print(f"  cmd={cmd} from {sender}: {len(datas)}x, data lens {dict(lens)}")
        interesting = list(dict.fromkeys(datas))[:3]
        for d in interesting:
            printable = ascii_or_hex(d)
            if printable:
                print(f"      e.g. '{printable}'")
            elif d:
                print(f"      e.g. {d.hex(' ')}")

    print()
    print("=" * 72)
    print(" alarmed: BAD CHECKSUMS")
    print("=" * 72)
    for k, v in bad.most_common():
        print(f"  cmd={k[0]} from {k[1]}: {v}x")
    if not bad:
        print("  none — clean capture")

    if keys:
        print()
        print("=" * 72)
        print("KEY FRAMES (panel button presses)")
        print("=" * 72)
        for k, v in keys.most_common():
            print(f"  {k}: {v}x")

    if rows:
        print()
        print("=" * 72)
        print("DISPLAY ROWSNAPSHOTS (last of each)")
        print("=" * 72)
        for cmd in sorted(rows):
            idx = {"51": "row 2", "52": "row 3", "53": "row 4"}[cmd]
            last = rows[cmd][-8:]
            for r in last:
                print(f"  {idx}: '{r}'")

    if gaps:
        print()
        print("=" * 72)
        print("GAP STATISTICS (rx silence lengths)")
        print("=" * 72)
        gaps.sort()
        print(f"  n={len(gaps)} min={gaps[0]}us  median={statistics.median(gaps):.0f}us  "
              f"p90={gaps[int(len(gaps)*0.9)]}us  max={gaps[-1]}us")
        inter = [g for g in gaps if g < 10000]
        if inter:
            print(f"  (excluding long pauses: median={statistics.median(inter):.0f}us)")

    print()
    counts = collections.Counter(addr for addr, _ in polls)
    if counts:
        print("polls counted:", dict(counts))
    return 0


def main() -> int:
    if len(sys.argv) == 2:
        text = open(sys.argv[1], errors="replace").read()
    else:
        text = sys.stdin.read()
    return analyze(text)


if __name__ == "__main__":
    sys.exit(main())
