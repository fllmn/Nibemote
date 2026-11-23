#!/usr/bin/env python3
"""Simulated VVM300 CPU (bus master) for the nibemote2 host build.

Connects to the firmware's tcp_uart server (default localhost:6638) and plays
the VVM300 side of the legacy Nibe bus protocol (docs/protocol.md):

    poll          : <addr:u16be>
    node response : 06 ACK | 05 ENQ (has data) | 15 NAK
    data frame    : <cmd> <sender:u16be> <len> <data...> <xor8 csum>
    close exchange : 03 ETX (sent by the frame's sender)

Exchange semantics per the protocol notes:
  - content lands ONLY in bare-ACK slots (protocol.md "The display
    slot") — the governor requests in bursts of 2 polls then answers 2
    polls with bare ACK
  - after the grant-ACK the node sends its frame; the master receipt-ACKs it;
    only then does the node send its ETX (sender-closes — protocol.md)
  - master->display frames ARE receipt-ACKed by the display (asymmetric)
  - a frame with a bad checksum gets NO reply at all (silence; never a rogue byte)

Boot model (the master owns the delay, not the display):
  - the display arms at setup and requests content ("40 88 00 00") on EVERY
    poll until the master delivers the FIRST content row (row 2 / cmd 51;
    rows 53/52 then arrive unprompted — protocol.md "The display slot")
  - an unready master still runs the bus: it polls the display and grants its
    requests, it just doesn't push content yet
  - after BOOT_DELAY_S the master is ready: menu request 0x62, then the rows

Scenario (asserted byte-exact):
  A. through the whole master boot delay every display poll answers ENQ with
     the request frame `40 00 F9 04 40 88 00 00 <chk>` (granted + receipt-ACKed)
  B. master ready: it pushes the menu request 0x62 into the still-requesting
     slot; the reply `62 ... 01 0F` surfaces behind the leftover request frames
  C. content, wire shape (protocol.md): row 0x50 pushed mid-request-phase
     (symbols don't end it — one more request cycle follows); row 2 (0x51) is
     the first content row and ends the phase: bare ACK, queued requests
     dropped; rows 4 (0x53) and 3 (0x52) arrive unprompted behind bare ACKs
  D. steady state: 0xFF keepalive, F2025 exchange (we stay silent in their
     slot), corrupted frame -> silence, Enter key at ~25 s through the ENQ flow
  E. queue-overflow regression: 12 clock queries queue 12 answers with polls
     paused (only grants drain, only polls grant) — the 8-slot outbound queue
     must keep exactly the first 8 (FIFO) and then drain back to bare-ACK

Usage: start the host firmware first (`esphome run configs/nibemote-host.yaml`),
then run this script. Prints one PASS/FAIL line per check; exits non-zero on any
failure.
"""

from __future__ import annotations

import socket
import sys
import time
from datetime import datetime
from zoneinfo import ZoneInfo

HOST = "127.0.0.1"
PORT = 6638

BOOT_DELAY_S = 3.0   # master boot (real VVM300 is much slower; shrunk for tests)
POLL_PERIOD_S = 0.1   # wire-ish bus cycle while polling through the boot

ADDR_VVM_CPU = 0x000A
ADDR_DISPLAY = 0x00F9
ADDR_F2025 = 0x41C9
ACK, ENQ, ETX = 0x06, 0x05, 0x03

failures = 0


def xor8(data: bytes) -> int:
    chk = 0
    for b in data:
        chk ^= b
    return chk


def frame(cmd: int, sender: int, data: bytes) -> bytes:
    body = bytes([cmd, (sender >> 8) & 0xFF, sender & 0xFF, len(data)]) + data
    return body + bytes([xor8(body)])


REQUEST_FRAME = frame(0x40, ADDR_DISPLAY, bytes.fromhex("40 88 00 00"))


class Master:
    def __init__(self) -> None:
        self.sock = socket.create_connection((HOST, PORT), timeout=10)
        self.sock.settimeout(10.0)
        self.buf = b""
        self.stray_etx = 0  # spontaneous ETX bytes from the display (must stay 0)

    def send(self, data: bytes) -> None:
        self.sock.sendall(data)
        time.sleep(0.01)  # engine rx-before-gap ordering inside one tick (matches wire push latency)

    def recv(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("firmware closed connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def poll(self, addr: int) -> int:
        self.send(bytes([(addr >> 8) & 0xFF, addr & 0xFF]))
        c = self.recv(1)[0]
        while c == ETX:
            # a close byte AT a poll boundary is a violation: the granted-frame
            # sender-closes (protocol.md) and that ETX is consumed by the
            # exchange helpers; a stray one here means we closed unsolicited
            self.stray_etx += 1
            c = self.recv(1)[0]
        return c

    def read_frame(self) -> tuple[bool, bytes]:
        hdr = self.recv(4)
        cmd, hi, lo, length = hdr
        body = self.recv(length + 1)
        raw = hdr + body
        return raw[-1] == xor8(raw[:-1]), raw

    @staticmethod
    def check(label: str, ok: bool, detail: str = "") -> None:
        global failures
        mark = "PASS" if ok else "FAIL"
        if not ok:
            failures += 1
        print(f"[{mark}] {label}{' — ' + detail if detail else ''}")


def expect_ack(m: Master, label: str) -> int:
    resp = m.poll(ADDR_DISPLAY)
    m.check(f"{label}: display answers ACK", resp == ACK, hex(resp))
    return resp


def expect_enq_frame(m: Master, label: str, expected: bytes) -> int:
    """We polled, the display wants to send: ENQ -> grant -> frame -> receipt ACK -> ETX."""
    resp = m.poll(ADDR_DISPLAY)
    m.check(f"{label}: display answers ENQ", resp == ENQ, hex(resp))
    if resp != ENQ:
        return resp
    m.send(bytes([ACK]))
    ok, raw = m.read_frame()
    m.check(f"{label}: frame checksum ok", ok, raw.hex(" "))
    m.check(f"{label}: frame bytes", raw == expected, f"got {raw.hex(' ')}, want {expected.hex(' ')}")
    # panel grammar (protocol.md): the master receipt-ACKs the node's frame,
    # and only then does the node send its ETX
    m.send(bytes([ACK]))
    etx = m.recv(1)[0]
    m.check(f"{label}: display closes with ETX (sender-closes)", etx == ETX, hex(etx))
    return resp


def push_after_enq(m: Master, label: str, cmd: int, data: bytes, max_bare: int = 6) -> None:
    """The display ENQs (still requesting); the ready master takes the slot and
    pushes a frame anyway — the display receipt-ACKs it and the exchange closes
    with the master's ETX. The announce governor breathes: the ENQ may be a
    couple of bare-ACK polls away, so wait for the next request burst."""
    for _ in range(max_bare + 2):
        resp = m.poll(ADDR_DISPLAY)
        if resp == ENQ:
            break
        m.check(f"{label}: waiting poll answers bare ACK", resp == ACK, hex(resp))
    else:
        m.check(f"{label}: display never ENQed", False)
        return
    m.send(frame(cmd, ADDR_VVM_CPU, data))
    ack = m.recv(1)[0]
    m.check(f"{label}: display receipt-ACKs the push", ack == ACK, hex(ack))
    m.send(bytes([ETX]))


def push_after_ack(m: Master, label: str, cmd: int, data: bytes) -> None:
    """Content pushed into a bare-ACK slot (protocol.md: the remaining rows
    land unprompted once the display has stopped requesting)."""
    resp = m.poll(ADDR_DISPLAY)
    m.check(f"{label}: display answers bare ACK", resp == ACK, hex(resp))
    m.send(frame(cmd, ADDR_VVM_CPU, data))
    ack = m.recv(1)[0]
    m.check(f"{label}: display receipt-ACKs the push", ack == ACK, hex(ack))
    m.send(bytes([ETX]))


def grant_until_frame(m: Master, label: str, expected: bytes, max_frames: int = 8) -> int:
    """Grant queued frames one by one (each is a request) until `expected`
    surfaces; asserts every intermediate frame is the request frame."""
    requests = 0
    for i in range(max_frames):
        resp = m.poll(ADDR_DISPLAY)
        if resp != ENQ:
            m.check(f"{label}: display answers ENQ", False, f"got {hex(resp)} at cycle {i}")
            return requests
        m.send(bytes([ACK]))
        ok, raw = m.read_frame()
        m.send(bytes([ACK]))
        etx = m.recv(1)[0]
        m.check(f"{label}: display closes with ETX (sender-closes)", etx == ETX, hex(etx))
        m.check(f"{label}: frame checksum ok", ok, raw.hex(" "))
        if raw == expected:
            m.check(f"{label}: leftover requests granted first were the request frame",
                    requests >= 0, f"{requests}")
            return requests
        m.check(f"{label}: queued frame #{i + 1} is the request frame", raw == REQUEST_FRAME,
                f"got {raw.hex(' ')}")
        requests += 1
    m.check(f"{label}: expected frame never surfaced", False)
    return requests


def wait_enq_frame(m: Master, label: str, expected: bytes, timeout: float = 30.0) -> None:
    """Poll until the display wants to send (it may bare-ACK at idle while waiting)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        resp = m.poll(ADDR_DISPLAY)
        if resp == ENQ:
            break
    else:
        m.check(f"{label}: display never ENQed", False)
        return
    m.send(bytes([ACK]))
    ok, raw = m.read_frame()
    m.check(f"{label}: frame checksum ok", ok, raw.hex(" "))
    m.check(f"{label}: frame bytes", raw == expected, f"got {raw.hex(' ')}, want {expected.hex(' ')}")
    m.send(bytes([ACK]))
    etx = m.recv(1)[0]
    m.check(f"{label}: display closes with ETX (sender-closes)", etx == ETX, hex(etx))


def clock_report_round(m: Master) -> None:
    """The master asks for the time (D4, ~64 s cadence on the real wire); the
    display answers with a 7-byte BCD snapshot of NOW (yy mm dd hh mm ss +
    tail). Both the firmware and this sim read the same host clock, so the
    decoded time must land within seconds; the tail is the configured byte."""
    resp = None
    for _ in range(30):  # the answer is a REAL frame: ENQ comes at once,
        resp = m.poll(ADDR_DISPLAY)  # never a governor breath poll
        if resp == ENQ:
            break
        m.check("clock round: waiting poll answers bare ACK", resp == ACK, hex(resp))
    m.check("clock round: display answers ENQ", resp == ENQ, hex(resp))
    if resp != ENQ:
        return
    m.send(bytes([ACK]))
    ok, raw = m.read_frame()
    m.send(bytes([ACK]))
    etx = m.recv(1)[0]
    m.check("clock reply: display closes with ETX (sender-closes)", etx == ETX, hex(etx))
    m.check("clock reply frame checksum", ok, raw.hex(" "))
    m.check("clock reply shape cmd 0xD4 len 7", raw[0] == 0xD4 and raw[3] == 7, raw.hex(" "))

    def un_bcd(b: int) -> int:
        return ((b >> 4) & 0x0F) * 10 + (b & 0x0F)

    yy, mo, dd, hh, mi, ss, tl = (un_bcd(b) for b in raw[4:11])
    # the host build has no time: platform -> the engine serves the raw system
    # epoch (UTC, no tz config possible); a device build with sntp +
    # timezone answers local time instead
    tz = ZoneInfo("UTC")
    # Sunday=1..Saturday=7 (python weekday: Mon=0..Sun=6)
    expected_dow = (datetime.now(tz).weekday() + 1) % 7 + 1
    m.check("clock reply tail = day-of-week", tl == expected_dow, f"got {tl}, want {expected_dow}")
    m.check("clock reply year", yy == datetime.now(tz).year - 2000, f"got {yy:02d}")
    answered = datetime(2000 + yy, mo, dd, hh, mi, ss, tzinfo=tz)
    now = datetime.now(tz)
    delta = abs((answered - now).total_seconds())
    # tolerate a midnight rollover between the two reads
    delta = min(delta, 86400 - delta)
    m.check("clock reply matches sim clock (±10 s)", delta <= 10,
            f"answered {answered.isoformat()} vs now {now.isoformat()}")


def master_sends(m: Master, label: str, cmd: int, data: bytes, sender: int = ADDR_VVM_CPU) -> None:
    """After the display ACKed our poll we push a frame; the display receipt-ACKs
    it; we close with ETX (the master closes its own exchange)."""
    m.send(frame(cmd, sender, data))
    ack = m.recv(1)[0]
    m.check(f"{label}: display receipt-ACKs our frame", ack == ACK, hex(ack))
    m.send(bytes([ETX]))


def expect_silence(m: Master, seconds: float = 1.0) -> bool:
    """Assert the display added no stray byte to the stream (idle-bus check)."""
    old = m.sock.gettimeout()
    m.sock.settimeout(seconds)
    silent = False
    try:
        b = m.sock.recv(1)
        if not b:
            raise ConnectionError("firmware closed connection")
        m.buf = b + m.buf
    except socket.timeout:
        silent = True
    finally:
        m.sock.settimeout(old)
    return silent


def queue_overflow_bist(m: Master, queries: int = 12) -> None:
    """Overfill the display's outbound queue and prove the fenceposts.

    queue_frame_ (engine TX_QUEUE_N = 8 slots over mod-16 indices) must reject
    beyond capacity; its original "full" test compared head+1 == tail (mod 2N
    = 16) which 8 pending entries never reach, so pushes past 8 aliased slot 0
    and later surfaced 12 frames newest-first. Each 0xD4 clock query queues
    exactly one answer frame, so `queries` back-to-back pushes with polls
    paused (nothing else can drain: only grants consume, only polls grant)
    fill it deterministically. Then resume polling and drain.

    Survival checks: capacity answers (FIFO — first-queued answers first,
    by their BCD time-of-day), all D4 snapshots, queue ends bare-ACK empty."""
    queued_at = []
    for i in range(queries):
        m.send(frame(0xD4, ADDR_VVM_CPU, b""))
        ack = m.recv(1)[0]
        m.check(f"overflow query #{i + 1}: display receipt-ACKs the push", ack == ACK, hex(ack))
        m.send(bytes([ETX]))
        queued_at.append(datetime.now(ZoneInfo("UTC")))
        time.sleep(1.1)  # consecutive answers carry different BCD seconds -> order is checkable

    drained = []
    for _ in range(queries + 2):
        resp = m.poll(ADDR_DISPLAY)
        if resp != ENQ:
            m.check("overflow drain: queue ends bare-ACK empty", resp == ACK, hex(resp))
            break
        m.send(bytes([ACK]))
        ok, raw = m.read_frame()
        m.send(bytes([ACK]))
        etx = m.recv(1)[0]
        n = len(drained) + 1
        m.check(f"overflow drain #{n}: frame checksum ok", ok, raw.hex(" "))
        m.check(f"overflow drain #{n}: display closes with ETX (sender-closes)", etx == ETX, hex(etx))
        drained.append(raw)
    else:
        m.check("overflow drain: queue did not empty within the poll budget", False)
        return

    def un_bcd(b: int) -> int:
        return ((b >> 4) & 0x0F) * 10 + (b & 0x0F)

    def tod_s(raw) -> int:  # hh:mm:ss of the snapshot (hour keeps FIFO across minute wraps)
        hh, mi, ss = (un_bcd(b) for b in raw[7:10])
        return hh * 3600 + mi * 60 + ss

    shape_ok = len(drained) > 0 and all(r[0] == 0xD4 and r[3] == 7 for r in drained)
    m.check("overflow drain: exactly 8 answers (TX_QUEUE_N) survive", len(drained) == 8,
            f"got {len(drained)}")
    m.check("overflow drain: survivors are all D4 clock snapshots", shape_ok)
    if len(drained) >= 2 and shape_ok:
        fifo = all(0 < (tod_s(drained[i + 1]) - tod_s(drained[i])) % 86400 <= 15
                   for i in range(len(drained) - 1))
        m.check("overflow drain: first-queued answers drain first (FIFO)", fifo)
        first = queued_at[0]
        delta = abs(tod_s(drained[0]) - (first.hour * 3600 + first.minute * 60 + first.second))
        delta = min(delta, 86400 - delta)
        m.check("overflow drain: the drain starts at query #1's answer", delta <= 4,
                f"first answer {delta} s from query #1")


def main() -> int:
    boot_t0 = time.monotonic()
    print(f"connecting to host firmware at {HOST}:{PORT} ...")
    m = Master()
    print("connected; master boot starts")

    # ---- A. master booting: polls the display and grants its content requests,
    #         but doesn't push anything (not ready). The display must keep
    #         requesting 40 88 00 00 on EVERY poll until rows 2-4 arrive.
    # the master only ever pushes content into bare slots — the panel requests
    # in short bursts and bare-ACKs the polls between (protocol.md).
    requests = 0
    breaths = 0
    bursts = 0
    while time.monotonic() - boot_t0 < BOOT_DELAY_S:
        bursts += 1
        expect_enq_frame(m, f"boot burst {bursts} request #1", REQUEST_FRAME)
        expect_enq_frame(m, f"boot burst {bursts} request #2", REQUEST_FRAME)
        expect_ack(m, f"boot burst {bursts} breath #1")
        expect_ack(m, f"boot burst {bursts} breath #2")
        requests += 2
        breaths += 2
        time.sleep(POLL_PERIOD_S)
    m.check("display requested in bursts with bare breaths through the master boot",
            requests >= 10 and breaths >= 6, f"{requests} requests, {breaths} breath polls")

    # ---- B. master ready: it takes the slot from the still-requesting display
    #         and pushes the menu request in; the reply surfaces behind the
    #         leftover request frames (granted one by one)
    push_after_enq(m, "menu request 0x62 into the ENQ slot", 0x62, b"")
    grant_until_frame(m, "menu reply 0F", frame(0x62, ADDR_DISPLAY, b"\x0f"), max_frames=8)

    # ---- C. content, wire shape (protocol.md): row 0x50 (symbols)
    #         lands mid-request-phase and must NOT end it; row 2 (0x51) is the
    #         first content row — the request phase must end EXACTLY there;
    #         rows 53/52 then follow unprompted behind bare ACKs
    push_after_enq(m, "display row 0x50 (symbols) pushed mid-request-phase", 0x50, b"\x01\x02\x03")
    expect_enq_frame(m, "still requesting after the symbols row", REQUEST_FRAME)
    push_after_enq(m, "display row 2 (0x51)", 0x51, b"51.9\xB0        ")
    # first content row in hand: bare ACK — request phase over, queued requests dropped
    expect_ack(m, "request phase ended at the first content row")
    push_after_ack(m, "display row 4 (0x53) unprompted", 0x53, b"1.0      14:25")
    push_after_ack(m, "display row 3 (0x52) unprompted", 0x52, b"Varmvattentemperatur")
    expect_ack(m, "steady state: bare ACK")

    # ---- D. steady state
    expect_ack(m, "keepalive poll")
    master_sends(m, "keepalive 0xFF", 0xFF, b"")

    # the master asks the panel for the time; d4_reply answers with a BCD
    # snapshot of our synced clock; the tail byte is the day-of-week
    # (Sunday=1 — protocol.md; the sim computes it live)
    master_sends(m, "clock query 0xD4", 0xD4, b"")
    clock_report_round(m)

    # an F2025 exchange happens in its own slot — we must stay completely silent
    m.send(bytes([ADDR_F2025 >> 8, ADDR_F2025 & 0xFF]))  # master polls the F2025
    m.send(bytes([ENQ]))                                 # the F2025 has telemetry
    m.send(bytes([ACK]))                                 # master grants
    telemetry = bytes.fromhex("08 34 01 77 03 17 0c 00 0e a0 01 f4 00 64 00 96 00 00 02")
    m.send(frame(0x65, ADDR_F2025, telemetry))            # the F2025's sensor block
    m.send(bytes([ETX]))                                  # the F2025 closes
    Master.check("our silence during the F2025 exchange", expect_silence(m, 0.8))
    expect_ack(m, "next own poll answered")

    # the firmware's on_boot script presses Enter ~25 s after boot
    wait_enq_frame(m, "key enter", frame(0x40, ADDR_DISPLAY, bytes([0x04, 0x88, 0x00, 0x00])),
                   timeout=30)  # data[1] = the relaid system byte (commands.md)

    # corrupted frame in our slot -> NO receipt ACK (silence), but the parser
    # stays boundary-anchored: the next poll is answered normally
    expect_ack(m, "pre-corruption poll")
    bad = frame(0x50, ADDR_VVM_CPU, bytes.fromhex("01 02 03"))
    bad = bad[:-1] + bytes([bad[-1] ^ 0xFF])
    m.send(bad)
    m.send(bytes([ETX]))  # master closes regardless; the panel receipt-ACKed nothing
    Master.check("corrupt frame gets no receipt ACK", expect_silence(m, 0.8))
    expect_ack(m, "poll after corruption")

    # the firmware's on_boot sets knob placement 200 (~40 s after boot):
    # the knob reaches the master as a cmd-0x40 event frame,
    # [03, C8, 00, 00] — the knob event (hypotheses.md); this bench check
    # pins the frame the engine emits
    wait_enq_frame(m, "knob placement 200",
                   frame(0x40, ADDR_DISPLAY, bytes([0x03, 0xC8, 0x00, 0x00])), timeout=40)

    # ---- E. outbound-queue overflow regression (queue_frame_ full check):
    #         12 clock answers queued with polls paused; exactly the first 8
    #         may survive, FIFO, and the queue must drain back to bare-ACK
    queue_overflow_bist(m)

    m.check("no spontaneous display ETX during the run", m.stray_etx == 0,
            f"got {m.stray_etx}")
    m.sock.close()
    print()
    if failures:
        print(f"FAILED: {failures} check(s) failed")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
