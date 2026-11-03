"""nibemote CLI.

Commands:
  run               run the daemon (bus + MQTT/Home Assistant bridge)
  listen            sniff the bus and print every poll/frame
  scan              discover which bus addresses the VVM300 polls
  params            collect 0xC0 parameter frames and print index -> value
  write <index> <value>  write a parameter (raw value) on next poll
  list-registers    dump the modbus-style register map (modern protocol)
"""

from __future__ import annotations

import logging
import sys
import time

import typer
from rich.console import Console
from rich.table import Table

from . import __version__, protocol as p
from .bus import SerialBus
from .client import NibeClient
from .config import get_settings
from .registers import load_registers

app = typer.Typer(help="Nibe F2025 + VVM300 RS485 remote controller (display replacement).")
console = Console()
log = logging.getLogger("nibemote")


def _setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )


def _parse_int(value: str) -> int:
    value = value.strip()
    if value.lower().startswith("0x"):
        return int(value, 16)
    return int(value, 10)


def _make_bus(settings) -> SerialBus:
    return SerialBus(
        port=settings.serial_port,
        baud=settings.baud,
        parity=settings.parity,
        stopbits=settings.stopbits,
        rs485_rts=settings.rs485_rts,
    )


def _open_bus(settings) -> SerialBus:
    bus = _make_bus(settings)
    try:
        bus.open()
    except Exception as exc:
        console.print(f"[red]Cannot open {settings.serial_port} at {settings.baud} "
                      f"{settings.parity}{settings.stopbits}: {exc}[/red]")
        console.print("Check NIBEMOTE_SERIAL_PORT / cabling / dialout group.")
        raise typer.Exit(2)
    return bus


@app.command()
def run() -> None:
    """Run the full daemon: answer the display, poll registers, publish to MQTT."""
    from .daemon import Daemon

    settings = get_settings()
    _setup_logging(settings.log_level)
    Daemon(settings).run()


@app.command()
def listen(duration: float = typer.Option(30.0, help="Seconds to listen")) -> None:
    """Passively sniff the bus and print every poll/frame (no ACKs sent)."""
    settings = get_settings()
    _setup_logging(settings.log_level)
    bus = _open_bus(settings)
    parser = p.LegacyParser()
    console.print(f"[green]Listening on {settings.serial_port} for {duration}s[/green]")
    deadline = time.monotonic() + duration
    try:
        while time.monotonic() < deadline:
            for ev in parser.feed(bus.read()):
                console.print(_format_legacy_event(ev))
    finally:
        bus.close()


@app.command()
def scan(duration: float = typer.Option(60.0, help="Seconds to scan")) -> None:
    """Discover which addresses the VVM300 polls, with which commands."""
    settings = get_settings()
    _setup_logging(settings.log_level)
    bus = _open_bus(settings)
    parser = p.LegacyParser()
    polled: dict[int, int] = {}
    cmds: dict[int, dict[int, int]] = {}
    params_seen: dict[int, bytes] = {}
    console.print(f"[green]Scanning bus for {duration}s...[/green]")
    deadline = time.monotonic() + duration
    try:
        while time.monotonic() < deadline:
            for ev in parser.feed(bus.read()):
                if isinstance(ev, p.Poll):
                    polled[ev.address] = polled.get(ev.address, 0) + 1
                elif isinstance(ev, p.LegacyFrame) and ev.checksum_ok:
                    cmds.setdefault(ev.sender, {}).setdefault(ev.cmd, 0)
                    cmds[ev.sender][ev.cmd] += 1
                    if ev.cmd == p.LEGACY_CMD_PARAMS:
                        params_seen.update(p.parse_legacy_params(ev.data))
    finally:
        bus.close()
    table = Table(title="Addresses polled on the bus")
    table.add_column("address", style="cyan")
    table.add_column("polls")
    table.add_column("meaning")
    meaning = {
        p.LEGACY_ADDR_VVM_CPU: "VVM300 CPU (master)",
        p.LEGACY_ADDR_RELAY: "relay board",
        p.LEGACY_ADDR_DISPLAY: "DISPLAY CARD <- impersonate this",
        p.LEGACY_ADDR_RCU: "RCU (external display)",
        p.LEGACY_ADDR_HEATPUMP_1: "F2025 heat pump",
    }
    for addr, n in sorted(polled.items()):
        table.add_row(hex(addr), str(n), meaning.get(addr, "?"))
    console.print(table)
    table2 = Table(title="Frames by sender / command")
    table2.add_column("sender", style="cyan")
    table2.add_column("commands (cmd: count)")
    for sender, c in sorted(cmds.items()):
        names = ", ".join(f"{hex(cmd)}: {n}" for cmd, n in sorted(c.items()))
        table2.add_row(hex(sender), names)
    console.print(table2)
    if params_seen:
        table3 = Table(title=f"Parameter indices seen in 0xC0 frames ({len(params_seen)})")
        table3.add_column("index")
        table3.add_column("raw")
        for idx, raw in sorted(params_seen.items()):
            table3.add_row(hex(idx), raw.hex(" "))
        console.print(table3)
    console.print("Set NIBEMOTE_ACK_ADDRESSES=0xF9 (display card) to take over the panel.")


@app.command()
def params(duration: float = typer.Option(15.0, help="Seconds to collect")) -> None:
    """Collect 0xC0 parameter frames and print index -> raw value."""
    settings = get_settings()
    _setup_logging(settings.log_level)
    bus = _open_bus(settings)
    client = NibeClient(bus, ack_addresses=[])  # passive: never ACK
    console.print(f"[green]Collecting parameter frames for {duration}s...[/green]")
    deadline = time.monotonic() + duration
    try:
        while time.monotonic() < deadline:
            client.poll()
            time.sleep(0.01)
    finally:
        bus.close()
    table = Table(title=f"{len(client.params)} parameter indices seen")
    table.add_column("index", style="cyan")
    table.add_column("raw")
    table.add_column("as s16/10")
    for idx, raw in sorted(client.params.items()):
        try:
            val = int.from_bytes(raw.ljust(2, b"\x00")[:2], "little", signed=True) / 10
        except Exception:
            val = None
        table.add_row(hex(idx), raw.hex(" "), f"{val}" if val is not None else "")
    console.print(table)


@app.command()
def write(
    index: str,
    value: str,
    timeout: float = typer.Option(10.0),
    force: bool = typer.Option(False, "--force", "-f", help="Allow unknown/read-only index"),
) -> None:
    """Write a parameter index (decimal or 0x hex) with an engineering-unit value.

    Only indices known and marked writable in params.py are allowed unless
    --force is given. Example: `nibemote write 0x0B -2` (curve offset).
    """
    from .params import get_param

    settings = get_settings()
    _setup_logging(settings.log_level)
    idx = _parse_int(index)
    param = get_param(idx)
    if param is None:
        if not force:
            console.print(f"[red]unknown parameter index {idx:#x}; use --force to write anyway[/red]")
            sys.exit(1)
        raw = int(_parse_int(value)).to_bytes(2, "big")
    else:
        if not param.writable and not force:
            console.print(f"[red]{param.name} ({idx:#x}) is read-only; use --force to override[/red]")
            sys.exit(1)
        try:
            raw = param.encode(value)
        except Exception as exc:
            console.print(f"[red]invalid value {value!r} for {param.name}: {exc}[/red]")
            sys.exit(1)
    bus = _open_bus(settings)
    client = NibeClient(bus, ack_addresses=settings.addresses)
    client.queue_write(idx, raw)
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            client.poll()
            time.sleep(0.02)
            if client._pending_write is None:
                with client._lock:
                    if not client._write_queue:
                        break
    finally:
        bus.close()
    name = param.name if param else "?"
    console.print(f"[green]queued write {name} ({idx:#04x}) = {value} -> {raw.hex(' ')}[/green] "
                  "(sent on the next poll from the VVM300)")


@app.command()
def press(
    buttons: list[str] = typer.Argument(..., help="Button codes to press, e.g. 02 04 01"),
    listen: float = typer.Option(25.0, help="Seconds to listen after boot announce"),
    gap: float = typer.Option(3.0, help="Seconds between button presses"),
) -> None:
    """Act as the display (0xF9): send boot announce, press buttons, print menu screens."""
    settings = get_settings()
    _setup_logging(settings.log_level)
    bus = _open_bus(settings)
    client = NibeClient(bus, ack_addresses=settings.addresses)
    client.queue_announce(0x40, bytes.fromhex("40 85 00 00"))
    rows: dict[int, str] = {}

    def on_event(ev) -> None:
        if isinstance(ev, p.LegacyFrame) and 0x50 <= ev.cmd <= 0x53:
            rows[ev.cmd - 0x4F] = ev.data.rstrip(b"\x00").decode("ascii", "replace")
            if ev.cmd == 0x53:
                console.print(f'  screen r2="{rows.get(2, "")}" r3="{rows.get(3, "")}" '
                              f'r4="{rows.get(4, "")}"')

    client.on_event = on_event
    codes = [_parse_int(b) for b in buttons]
    console.print(f"[green]Active as display, pressing {buttons}[/green]")
    t0 = time.monotonic()
    pressed = 0
    try:
        while time.monotonic() - t0 < listen:
            client.poll()
            if pressed < len(codes) and time.monotonic() - t0 > 4 + pressed * gap:
                console.print(f">>> press {codes[pressed]:#04x}")
                client.press_button(codes[pressed])
                pressed += 1
            time.sleep(0.002)
    finally:
        console.print(f"stats: {client.stats}")
        bus.close()


@app.command()
def knob(
    position: str = typer.Argument(..., help="Knob position 0-255 (hex or dec); wraps"),
    listen: float = typer.Option(10.0, help="Seconds to listen after sending"),
) -> None:
    """Turn the temperature-adjustment knob: sends cmd 0x40 data [0x40, pos, 0, 0]."""
    settings = get_settings()
    _setup_logging(settings.log_level)
    bus = _open_bus(settings)
    client = NibeClient(bus, ack_addresses=settings.addresses)
    client.queue_announce(0x40, bytes.fromhex("40 85 00 00"))
    pos = _parse_int(position) & 0xFF
    console.print(f"[green]knob -> {pos:#04x}[/green]")
    t0 = time.monotonic()
    sent = False
    try:
        while time.monotonic() - t0 < listen:
            client.poll()
            if not sent and time.monotonic() - t0 > 3:
                client.queue_frame(0x40, bytes([0x40, pos, 0x00, 0x00]))
                sent = True
            time.sleep(0.002)
    finally:
        bus.close()


@app.command(name="list-registers")
def list_registers(writable: bool = typer.Option(False, "--writable", "-w")) -> None:
    """Dump the register map as a table."""
    settings = get_settings()
    registers = load_registers(settings.registers_file)
    table = Table(title=f"{len(registers)} registers (VVM map)")
    for col in ("address", "name", "title", "unit", "size", "factor", "rw", "min", "max"):
        table.add_column(col)
    for reg in sorted(registers.values(), key=lambda r: r.address):
        if writable and not reg.writable:
            continue
        table.add_row(
            str(reg.address), reg.name, reg.title, reg.unit, reg.size,
            str(reg.factor), "rw" if reg.writable else "ro",
            str(reg.min) if reg.min is not None else "", str(reg.max) if reg.max is not None else "",
        )
    console.print(table)


def _format_legacy_event(ev) -> str:
    if isinstance(ev, p.Poll):
        return f"[cyan]POLL[/cyan] addr={ev.address:#06x}"
    if isinstance(ev, p.LegacyFrame):
        return (f"[blue]FRAME[/blue] cmd={ev.cmd:#04x} from={ev.sender:#06x} "
                f"len={len(ev.data)} chk={'ok' if ev.checksum_ok else 'BAD'} "
                f"data={ev.data.hex(' ')}")
    if isinstance(ev, p.ControlByte):
        name = {p.LEGACY_ACK: "ACK", p.LEGACY_NAK: "NAK", p.LEGACY_ENQ: "ENQ"}.get(
            ev.value, hex(ev.value))
        return f"[yellow]{name}[/yellow]"
    if isinstance(ev, p.Etx):
        return "[dim]ETX[/dim]"
    return f"[dim]. {ev.value:02x}[/dim]"


@app.callback()
def main(version: bool = typer.Option(False, "--version", help="Show version and exit")) -> None:
    if version:
        console.print(f"nibemote {__version__}")
        raise typer.Exit()


if __name__ == "__main__":
    app()
