"""Bus client for the legacy VVM300/F2025 bus (see protocol.py).

The VVM300 CPU (address 0x000A) is the master: it polls nodes with a 2-byte
address; a polled node answers ACK (06) or ENQ (05). To send data (e.g. a
parameter write) we answer ENQ when polled, wait for the master's ACK, then
send a 0xC0 frame; the master ACKs and we close with ETX (03).

Parameter frames (cmd 0xC0) carry `00 <index> <value:1-2B>` groups — the same
parameter indices FredRovers documented in forum thread t=13714.
"""

from __future__ import annotations

import logging
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable

from . import protocol as p
from .bus import SerialBus

log = logging.getLogger(__name__)


@dataclass
class ParamUpdate:
    index: int
    raw: bytes
    source: str  # "frame" | "write_ack"


class NibeClient:
    def __init__(
        self,
        bus: SerialBus,
        ack_addresses: list[int] | None = None,
        on_param: Callable[[ParamUpdate], None] | None = None,
        on_event: Callable[[object], None] | None = None,
        heartbeat: bool = False,
    ) -> None:
        self.bus = bus
        self.ack_addresses = set(ack_addresses or [p.LEGACY_ADDR_DISPLAY])
        self.heartbeat = heartbeat  # answer every poll with ENQ + 0x40 heartbeat frame
        self.on_param = on_param
        self.on_event = on_event
        self.parser = p.LegacyParser()
        self._write_queue: deque[tuple[int, bytes]] = deque()  # (param index, raw value)
        self._pending_write: tuple[int, bytes] | None = None
        self._pending_announce: tuple[int, bytes] | None = None
        self._announce: tuple[int, bytes] | None = None  # (cmd, data) sent once via ENQ
        self._frame_queue: deque[tuple[int, bytes]] = deque()
        self._await_master_ack = False  # we sent ENQ, waiting for master ACK
        self._await_frame_ack = False  # we sent a frame, waiting for its ACK -> then ETX
        self._boot_announces = 0  # announce 0x40 on this many polls after cmd 0x22
        self._last_poll_was_ours = False
        self._lock = threading.Lock()
        self.params: dict[int, bytes] = {}
        self._stop = threading.Event()
        self.stats = {"polls": 0, "acks_sent": 0, "naks_sent": 0,
                      "frames": 0, "bad_checksum": 0}

    # -- public API ------------------------------------------------------

    def queue_write(self, index: int, raw_value: bytes) -> None:
        with self._lock:
            self._write_queue.append((index, raw_value))

    def queue_announce(self, cmd: int = 0x40, data: bytes = bytes.fromhex("40 85 00 00")) -> None:
        """Queue a boot-style announcement: on the next poll we answer ENQ and
        send this frame (the real display sends cmd 0x40 '40 85 00 00' at boot)."""
        with self._lock:
            self._announce = (cmd, data)

    def queue_frame(self, cmd: int, data: bytes) -> None:
        """Queue an arbitrary frame to send via ENQ on the next poll
        (e.g. a display button press: cmd 0x40, data [button, 0x85, 0, 0])."""
        with self._lock:
            if self._announce is None:
                self._announce = (cmd, data)
            else:
                self._frame_queue.append((cmd, data))

    def press_button(self, button: int) -> None:
        """Send a display button press (cmd 0x40)."""
        self.queue_frame(0x40, bytes([button, 0x85, 0x00, 0x00]))

    def stop(self) -> None:
        self._stop.set()

    def poll(self, timeout: float = 0.05) -> list:
        """Read available bytes, parse and dispatch. Returns events seen."""
        try:
            data = self.bus.read()
        except Exception:
            log.exception("serial read failed")
            return []
        if not data:
            return []
        events = self.parser.feed(data)
        for ev in events:
            self._dispatch(ev)
        return events

    def run_forever(self, idle_sleep: float = 0.002) -> None:
        log.info("Listening on the bus, answering polls for: %s",
                 ", ".join(hex(a) for a in sorted(self.ack_addresses)))
        while not self._stop.is_set():
            self.poll()
            time.sleep(idle_sleep)  # keep tiny: the master expects ACK within ms

    # -- dispatch ---------------------------------------------------------

    def _dispatch(self, ev) -> None:
        if self.on_event is not None:
            try:
                self.on_event(ev)
            except Exception:
                log.exception("on_event callback failed")
        if isinstance(ev, p.Poll):
            self.stats["polls"] += 1
            self._handle_poll(ev)
        elif isinstance(ev, p.LegacyFrame):
            self.stats["frames"] += 1
            self._handle_frame(ev)
        elif isinstance(ev, p.ControlByte):
            self._handle_control(ev)

    def _handle_poll(self, ev: p.Poll) -> None:
        self._last_poll_was_ours = ev.address in self.ack_addresses
        if not self._last_poll_was_ours:
            return  # not ours; just sniffed
        with self._lock:
            pending = self._write_queue.popleft() if self._write_queue else None
            announce = self._announce
            if announce is None and self._frame_queue:
                announce = self._frame_queue.popleft()
            self._announce = None
            if announce is None and pending is None:
                if self._boot_announces > 0:
                    self._boot_announces -= 1
                    announce = (0x40, bytes.fromhex("40 85 00 00"))
                elif self.heartbeat:
                    announce = (0x40, bytes.fromhex("40 85 00 00"))
        if announce is not None:
            self._pending_announce = announce
            self._await_master_ack = True
            self._send_byte(p.LEGACY_ENQ)  # master's ACK to our ENQ arrives at _RESP1
            log.info("ENQ -> boot announce cmd=%#x", announce[0])
        elif pending is not None:
            # signal "I have data" and wait for the master to ACK before sending
            self._pending_write = pending
            self._await_master_ack = True
            self._send_byte(p.LEGACY_ENQ)
            log.info("ENQ -> queued write of param %#04x", pending[0])
        else:
            self._send_byte(p.LEGACY_ACK)
            self.parser.note_self_response()  # our ACK is not echoed; next RX is master's frame/ETX

    def _handle_control(self, ev: p.ControlByte) -> None:
        if self._await_master_ack and ev.value == p.LEGACY_ACK:
            # master accepted our ENQ: send the queued frame now
            self._await_master_ack = False
            self._await_frame_ack = True
            if self._pending_write is not None:
                index, raw = self._pending_write
                self._pending_write = None
                payload = bytes([0x00, index]) + raw
                frame = p.encode_legacy_frame(p.LEGACY_CMD_PARAMS, self._our_address(), payload)
                self.bus.write(frame)
                self.parser.note_sent_frame()
                log.info("sent param write %#04x = %s", index, raw.hex(" "))
            else:
                cmd, data = self._pending_announce
                self._pending_announce = None
                frame = p.encode_legacy_frame(cmd, self._our_address(), data)
                self.bus.write(frame)
                self.parser.note_sent_frame()
                log.info("sent announce cmd=%#x data=%s", cmd, data.hex(" "))
        elif self._await_frame_ack and ev.value == p.LEGACY_ACK:
            # our frame was accepted: close the exchange with ETX (sender closes)
            self._await_frame_ack = False
            self._send_byte(p.LEGACY_ETX)

    def _handle_frame(self, ev: p.LegacyFrame) -> None:
        if not ev.checksum_ok:
            self.stats["bad_checksum"] += 1
            log.warning("bad checksum: %s", ev.raw.hex(" "))
            # only NAK if the frame was addressed to us (we can't tell from the
            # frame itself; the poll just before tells us — be conservative and
            # NAK only if the last poll was ours)
            if self._last_poll_was_ours:
                self._send_byte(p.LEGACY_NAK)
            return
        if ev.cmd == 0x22 and ev.sender == 0x000A and self._last_poll_was_ours:
            # master's boot ping: announce 0x40 on the next few polls, then plain-ACK
            # (the master sends 0x62 only after a plain-ACK poll following announces)
            self._boot_announces = 3
            log.info("cmd 0x22 -> announce 0x40 on next 3 polls, then plain ACK")
        elif ev.cmd == 0x62 and ev.sender == 0x000A and self._last_poll_was_ours:
            # master asks for the menu: reply with frame 62 0f on the next poll
            self.queue_frame(0x62, b"\x0f")
            log.info("cmd 0x62 -> queued reply 62 0f")
        if ev.cmd == p.LEGACY_CMD_PARAMS:
            for index, raw in p.parse_legacy_params(ev.data).items():
                self.params[index] = raw
                if self.on_param is not None:
                    try:
                        self.on_param(ParamUpdate(index, raw, "frame"))
                    except Exception:
                        log.exception("on_param callback failed")
        # ACK the frame if it followed a poll to us
        if self._last_poll_was_ours:
            self._send_byte(p.LEGACY_ACK)

    def _our_address(self) -> int:
        return sorted(self.ack_addresses)[0] if self.ack_addresses else p.LEGACY_ADDR_DISPLAY

    def _send_byte(self, b: int) -> None:
        self.bus.write(bytes([b]))
        if b == p.LEGACY_ACK:
            self.stats["acks_sent"] += 1
        elif b == p.LEGACY_NAK:
            self.stats["naks_sent"] += 1

    # -- one-shot helpers ---------------------------------------------------

    def read_params(self, timeout: float = 10.0) -> dict[int, bytes]:
        """Passively collect parameter frames for `timeout` seconds."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll(timeout=0.05)
            time.sleep(0.01)
        return dict(self.params)
