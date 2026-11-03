"""menuwalk — test app: virtual display + full menu dump.

Runs as the display (0xF9) on the VVM300 bus, renders the 4 display rows live
in the terminal (the "virtual display"), and walks the entire menu tree with
the captured button codes, dumping every screen to JSON + markdown.

Menu types (manual 8.1.1 "Menytyp"): Normal / Utökad / Service — the walker
cycles through all three and dumps each.

How to run:
  1. stop the daemon (it holds the port):  pkill -f "nibemote run"  (or stop the bg process)
  2. unplug the physical display from the VVM300
  3. start this app, THEN power-cycle the VVM300 (boot handshake needs it):
       cd /home/mans/proj/nibemote
       uv run python tools/menuwalk.py --out docs/menu-dump.json
"""

from __future__ import annotations

import argparse
import json
import re
import time
from pathlib import Path

from rich.console import Console
from rich.panel import Panel
from rich.live import Live

from nibemote import protocol as p
from nibemote.bus import SerialBus
from nibemote.client import NibeClient

BTN_BACK = 0x01
BTN_NEXT = 0x02
BTN_ENTER = 0x04
# remaining buttons seen in capture: 0x08, 0x10, 0x20 (semantics TBD)
BTN_08 = 0x08
BTN_10 = 0x10
BTN_20 = 0x20

MENU_TYPES = ["Normal", "Utökad", "Service"]

console = Console()


class VirtualDisplay:
    """Renders the 4 display rows (cmd 0x50-0x53) as a live terminal panel.
    Falls back to plain prints when stdout is not a TTY."""

    def __init__(self) -> None:
        self.rows = {1: "", 2: "", 3: "", 4: ""}
        self._live: Live | None = None
        self._tty = console.is_terminal
        self._last_text = ""

    def on_frame(self, ev) -> None:
        if isinstance(ev, p.LegacyFrame) and 0x50 <= ev.cmd <= 0x53:
            self.rows[ev.cmd - 0x4F] = ev.data.rstrip(b"\x00").decode("ascii", "replace")
            self.refresh()

    def text(self) -> str:
        return "\n".join(self.rows[i].ljust(21)[:21] for i in (1, 2, 3, 4))

    def start(self) -> None:
        if self._tty:
            self._live = Live(Panel(self.text(), title="VVM300 virtual display", border_style="cyan"),
                              console=console, refresh_per_second=8)
            self._live.start()

    def refresh(self) -> None:
        t = self.text()
        if self._live:
            self._live.update(Panel(t, title="VVM300 virtual display", border_style="cyan"))
        elif self._tty is False and t != self._last_text and any(t.split("\n")):
            # non-TTY: print the panel once per change
            console.print("+--- VVM300 display ---")
            for line in t.split("\n"):
                console.print("| " + line + " |")
            console.print("+----------------------")
            self._last_text = t

    def stop(self) -> None:
        if self._live:
            self._live.stop()


def menu_number(row4: str) -> str:
    m = re.match(r"\s*(\d+(?:\.\d+)*)", row4)
    return m.group(1) if m else ""


class Walker:
    def __init__(self, client: NibeClient, disp: VirtualDisplay, settle: float = 0.9) -> None:
        self.client = client
        self.disp = disp
        self.settle = settle
        self.dump: dict = {}  # menu -> {"label":..., "value":..., "type":...}

    # -- low level ---------------------------------------------------------
    def pump(self, secs: float) -> None:
        t0 = time.time()
        while time.time() - t0 < secs:
            self.client.poll()
            time.sleep(0.002)

    def screen(self) -> dict:
        r = self.disp.rows
        return {"menu": menu_number(r[4]), "label": r[3].strip(), "value": r[2].strip(),
                "row4": r[4].strip()}

    def press(self, btn: int, wait: float | None = None) -> dict:
        self.client.press_button(btn)
        self.pump(wait if wait is not None else self.settle)
        return self.screen()

    def wait_menu(self, old: str, timeout: float = 4.0) -> dict:
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.pump(0.15)
            s = self.screen()
            if s["menu"] and s["menu"] != old:
                return s
        return self.screen()

    # -- menu walking --------------------------------------------------------
    def record(self, menu_type: str, s: dict) -> None:
        if not s["menu"]:
            return
        key = f'{s["menu"]}'
        self.dump.setdefault(menu_type, {})
        # keep first label, update value (values change live)
        e = self.dump[menu_type].setdefault(key, {"label": s["label"], "value": s["value"]})
        if s["value"]:
            e["value"] = s["value"]
        if s["label"]:
            e["label"] = s["label"]

    def walk_level(self, menu_type: str, max_items: int = 40) -> None:
        """Scan all items at the current level with NEXT; enter submenus with ENTER."""
        seen: set[str] = set()
        for _ in range(max_items):
            s = self.screen()
            if not s["menu"] or s["menu"] in seen:
                break
            seen.add(s["menu"])
            self.record(menu_type, s)
            console.print(f"  [dim]{menu_type}[/dim] {s['menu']:<8} {s['label']:<36} {s['value']}")
            # try to enter a submenu
            s2 = self.press(BTN_ENTER)
            if s2["menu"] and s2["menu"] != s["menu"] and s2["menu"].startswith(s["menu"] + "."):
                self.walk_level(menu_type, max_items)  # recurse into submenu
                self.press(BTN_BACK, wait=1.2)  # come back up
                s = self.screen()
            else:
                # leaf or edit mode: back out without changing anything
                self.press(BTN_BACK, wait=0.8)
            # next sibling
            s3 = self.press(BTN_NEXT)
            if s3["menu"] in seen or not s3["menu"]:
                break

    def goto_menu(self, target: str, timeout: float = 30.0) -> bool:
        """Navigate from anywhere to a top-level menu like '8.0' using NEXT/BACK."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            s = self.screen()
            cur = s["menu"]
            if cur == target:
                return True
            top = cur.split(".")[0] + ".0"
            if cur == target:
                return True
            # crude: if we're deeper than target, go back; else next
            if cur and target and cur.split(".")[0] != target.split(".")[0]:
                self.press(BTN_BACK, wait=0.7)
            else:
                self.press(BTN_NEXT, wait=0.7)
        return False

    def set_menu_type(self, mtype: str) -> None:
        """Go to 8.1.1 Menytyp and select mtype (Normal/Utökad/Service)."""
        console.print(f"[yellow]setting Menytyp -> {mtype}[/yellow]")
        if not self.goto_menu("8.0"):
            console.print("[red]could not reach 8.0[/red]")
            return
        self.press(BTN_ENTER, wait=1.0)  # into 8.x
        # scan 8.x until 8.1.1
        for _ in range(10):
            s = self.screen()
            if s["menu"] == "8.1.1":
                break
            if s["menu"].startswith("8.1"):
                self.press(BTN_ENTER, wait=0.8)
            self.press(BTN_NEXT, wait=0.8)
        s = self.screen()
        if s["menu"] != "8.1.1":
            console.print(f"[red]not at 8.1.1 (at {s['menu']})[/red]")
            return
        self.press(BTN_ENTER, wait=1.0)  # enter edit mode
        for _ in range(6):
            s = self.screen()
            if f"@{mtype}@" in s["value"] or mtype in s["value"]:
                break
            self.press(BTN_NEXT, wait=0.8)
        self.press(BTN_ENTER, wait=1.2)  # confirm
        self.press(BTN_BACK, wait=1.0)
        self.press(BTN_BACK, wait=1.0)

    def run(self) -> dict:
        for mt in MENU_TYPES:
            console.print(f"[bold green]=== menu type: {mt} ===[/bold green]")
            self.set_menu_type(mt)
            self.pump(2.0)
            # go to top level 1.0
            for _ in range(12):
                s = self.screen()
                if s["menu"] == "1.0":
                    break
                self.press(BTN_BACK, wait=0.6)
            self.walk_level(mt)
        return self.dump


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB1")
    ap.add_argument("--out", default="docs/menu-dump.json")
    ap.add_argument("--settle", type=float, default=0.9)
    ap.add_argument("--no-display", action="store_true", help="don't render live panel")
    args = ap.parse_args()

    bus = SerialBus(args.port, rs485_rts=False, read_timeout=0.05)
    bus.open()
    client = NibeClient(bus, ack_addresses=[0x00F9])
    client.queue_announce(0x40, bytes.fromhex("40 85 00 00"))  # boot announce
    disp = VirtualDisplay()
    def on_event(ev):
        disp.on_frame(ev)
        if isinstance(ev, p.LegacyFrame) and ev.cmd not in (0xFF, 0x72, 0x55, 0x45, 0x76, 0xA1, 0xC4, 0x71, 0x65):
            console.print(f"[dim]frame cmd={ev.cmd:#04x} from={ev.sender:#06x} data={ev.data.hex(' ')}[/dim]")
    client.on_event = on_event
    if not args.no_display:
        disp.start()

    walker = Walker(client, disp, settle=args.settle)
    console.print("[green]menuwalk running — power-cycle the VVM300 now; waiting for boot...[/green]")
    try:
        # the client's own 0x22 handler does the boot announce sequence;
        # here we just wait (up to 150s) for the first menu screen to appear
        t0 = time.time()
        while time.time() - t0 < 300:
            client.poll()
            if walker.screen()["menu"]:
                break
            time.sleep(0.02)
        console.print(f"[yellow]first screen: {walker.screen()}[/yellow]")
        walker.pump(3.0)
        dump = walker.run()
    finally:
        disp.stop()
        bus.close()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(dump, indent=2, ensure_ascii=False))
    # markdown rendering
    md = ["# VVM300 menu dump", ""]
    for mt, items in dump.items():
        md.append(f"## Menytyp: {mt}")
        md.append("")
        md.append("| menu | label | value |")
        md.append("|---|---|---|")
        for menu in sorted(items, key=lambda m: [int(x) for x in m.split(".")]):
            e = items[menu]
            md.append(f"| {menu} | {e['label']} | {e['value']} |")
        md.append("")
    out.with_suffix(".md").write_text("\n".join(md))
    console.print(f"[bold green]dumped {sum(len(v) for v in dump.values())} screens -> {out} and {out.with_suffix('.md')}[/bold green]")


if __name__ == "__main__":
    main()
