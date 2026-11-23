#!/usr/bin/env python3
"""Command inventory over a µs-tail sniffer capture.

Extract every frame cmd with sender, the poll slot it landed in, n, payload
lengths and up to 4 distinct sample payloads. Slot attribution follows the
last poll event before the frame (the exchange it landed in).

    python3 tools/cmd_inventory.py captures/live-9.txt
"""
import re
import sys
from collections import defaultdict

PAT = re.compile(r"^\[\d+:\d+:\d+\.\d+\]\[[DIWC]\]\[nibe:\d+\]: (.*?)(?:\s+@(\d+) \(\+(\d+)\))?\s*$")


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "captures/live-9.txt"
    last_poll = None
    stats = defaultdict(lambda: {"n": 0, "lens": set(), "samples": []})
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PAT.match(line.rstrip())
            if not m:
                continue
            ev = m.groups()[0]
            if ev.startswith("poll "):
                last_poll = re.search(r"poll ([0-9A-Fa-f]{4})", ev).group(1).upper()
            elif ev.startswith("frame cmd="):
                cmd = int(re.search(r"cmd=([0-9A-Fa-f]{2})", ev).group(1), 16)
                sender = re.search(r"sender=([0-9A-Fa-f]{4})", ev).group(1).upper()
                ln = int(re.search(r"len=(\d+)", ev).group(1))
                data = re.search(r"data=(.*)$", ev).group(1).strip()
                key = (cmd, sender, last_poll)
                st = stats[key]
                st["n"] += 1
                st["lens"].add(ln)
                if len(st["samples"]) < 4 and data not in st["samples"]:
                    st["samples"].append(data)
    rows = sorted(stats.items(), key=lambda kv: (kv[0][2] or "", kv[0][1], kv[0][0]))
    print(f"{'cmd':>4} {'sender':>6} {'slot':>6} {'n':>5}  lens            samples")
    for (cmd, sender, poll), st in rows:
        samples = "; ".join(x[:40] for x in st["samples"][:3])
        print(f"0x{cmd:02X} {sender:>6} {str(poll):>6} {st['n']:>5}  {str(sorted(st['lens'])):15s} {samples}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
