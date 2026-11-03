#!/usr/bin/env python3
"""Simulated VVM300 CPU (bus master) for the ESPHome host build of nibemote.

Connects to the firmware's tcp_uart server (default localhost:6638) and plays
the VVM300 side of the legacy bus protocol (see src/nibemote/protocol.py):

    poll          : <addr:u16be>
    node response : 06 ACK | 05 ENQ (has data) | 15 NAK
    data frame    : <cmd> <sender:u16be> <len> <data...> <xor8 csum>
    end exchange  : 03 ETX

The scenario walks the full exchange: boot announce, boot ping 0x22, menu
request 0x62 + reply, a 0xC0 parameter dump (sensors), a parameter write
(triggered by the firmware's sim_actions script: curve offset -2), a display
button press (cmd 0x40) and the display text rows (cmd 0x50-0x53).

Usage: start the host firmware first (esphome run esphome/nibemote-host.yaml),
then run this script. It prints one PASS/FAIL line per check and exits
non-zero on any failure.
"""

from __future__ import annotations

import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 6638

ADDR_VVM_CPU = 0x000A
ADDR_DISPLAY = 0x00F9
ACK, NAK, ENQ, ETX = 0x06, 0x15, 0x05, 0x03

failures = 0


def xor8(data: bytes) -> int:
    chk = 0
    for b in data:
        chk ^= b
    return chk


def frame(cmd: int, sender: int, data: bytes) -> bytes:
    body = bytes([cmd, (sender >> 8) & 0xFF, sender & 0xFF, len(data)]) + data
    return body + bytes([xor8(body)])


class Master:
    def __init__(self) -> None:
        self.sock = socket.create_connection((HOST, PORT), timeout=10)
        self.sock.settimeout(10.0)
        self.buf = b""

    def send(self, data: bytes) -> None:
        self.sock.sendall(data)
        time.sleep(0.15)  # let the firmware loop run

    def recv(self, n: int) -> bytes:
        """Read exactly n bytes."""
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("firmware closed connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def poll(self, addr: int) -> int:
        self.send(bytes([(addr >> 8) & 0xFF, addr & 0xFF]))
        return self.recv(1)[0]

    def read_frame(self) -> tuple[bool, bytes]:
        hdr = self.recv(4)
        cmd, hi, lo, length = hdr
        sender = (hi << 8) | lo
        body = self.recv(length + 1)
        raw = hdr + body
        ok = raw[-1] == xor8(raw[:-1])
        return ok, raw

    def check(self, label: str, ok: bool, detail: str = "") -> None:
        global failures
        mark = "PASS" if ok else "FAIL"
        if not ok:
            failures += 1
        print(f"[{mark}] {label}{' — ' + detail if detail else ''}")


def expect_enq_frame(m: Master, label: str, expected: bytes) -> None:
    """We polled, the node wants to send: ENQ -> ACK -> frame -> ACK -> ETX."""
    resp = m.poll(ADDR_DISPLAY)
    m.check(f"{label}: node answers ENQ", resp == ENQ, hex(resp))
    m.send(bytes([ACK]))
    ok, raw = m.read_frame()
    m.check(f"{label}: frame checksum ok", ok, raw.hex(" "))
    m.check(f"{label}: frame bytes", raw == expected, f"got {raw.hex(' ')}, want {expected.hex(' ')}")
    m.send(bytes([ACK]))
    etx = m.recv(1)[0]
    m.check(f"{label}: node closes with ETX", etx == ETX, hex(etx))


def wait_enq_frame(m: Master, label: str, expected: bytes, timeout: float = 30.0) -> None:
    """Poll until the node wants to send (it may just ACK for a while), then
    run the usual ENQ -> ACK -> frame -> ACK -> ETX exchange."""
    deadline = time.monotonic() + timeout
    resp = m.poll(ADDR_DISPLAY)
    while resp != ENQ and time.monotonic() < deadline:
        if resp not in (ACK,):
            m.check(f"{label}: unexpected poll response", False, hex(resp))
            return
        resp = m.poll(ADDR_DISPLAY)
    m.check(f"{label}: node answers ENQ", resp == ENQ, hex(resp))
    if resp != ENQ:
        return
    m.send(bytes([ACK]))
    ok, raw = m.read_frame()
    m.check(f"{label}: frame checksum ok", ok, raw.hex(" "))
    m.check(f"{label}: frame bytes", raw == expected, f"got {raw.hex(' ')}, want {expected.hex(' ')}")
    m.send(bytes([ACK]))
    etx = m.recv(1)[0]
    m.check(f"{label}: node closes with ETX", etx == ETX, hex(etx))


def expect_ack(m: Master, label: str) -> None:
    resp = m.poll(ADDR_DISPLAY)
    m.check(f"{label}: node answers ACK", resp == ACK, hex(resp))


def master_sends(m: Master, label: str, cmd: int, data: bytes) -> None:
    """After the node ACKed our poll we push a frame; node ACKs; we send ETX."""
    m.send(frame(cmd, ADDR_VVM_CPU, data))
    ack = m.recv(1)[0]
    m.check(f"{label}: node ACKs our frame", ack == ACK, hex(ack))
    m.send(bytes([ETX]))


def main() -> int:
    print(f"connecting to host firmware at {HOST}:{PORT} ...")
    m = Master()
    print("connected")

    # 1. boot announce queued by the firmware at setup()
    expect_enq_frame(m, "boot announce", frame(0x40, ADDR_DISPLAY, bytes.fromhex("40 85 00 00")))

    # 2. plain ACK poll, then boot ping 0x22
    expect_ack(m, "plain poll")
    master_sends(m, "boot ping 0x22", 0x22, bytes.fromhex("85 00"))

    # 3. boot ping arms 3 more announces; consume them
    for i in range(3):
        expect_enq_frame(m, f"announce #{i + 1} after 0x22", frame(0x40, ADDR_DISPLAY, bytes.fromhex("40 85 00 00")))

    # 4. menu request 0x62 -> firmware replies '62 0f' on the next poll
    expect_ack(m, "menu poll")
    master_sends(m, "menu request 0x62", 0x62, b"")
    expect_enq_frame(m, "menu reply 62 0f", frame(0x62, ADDR_DISPLAY, b"\x0f"))

    # 5. parameter dump -> sensors (firmware logs each decoded param)
    expect_ack(m, "params poll")
    params = bytes.fromhex(
        "00 00 07"          # cpu_id = 7
        "00 01 01 f4"        # outdoor_temp = 50.0 °C
        "00 02 00 d9"        # hot_water = 21.7 °C
        "00 06 00 8c"        # supply_temp = 14.0 °C
        "00 08 00 96"        # compressor_temp = 15.0 °C
        "00 0b 02"           # curve_offset = 2
        "00 13 02"           # compressor_on = on
        "00 1a 00 64"        # compressor_starts = 100
    )
    master_sends(m, "param dump 0xC0", 0xC0, params)

    # 6. display text rows: cmd 0x50..0x53 = rows 1..4, null-terminated ASCII
    #    (observed layout: row2 = value, row3 = label, row4 = menu + clock).
    #    Each frame gets its own poll exchange (POLL -> ACK -> FRAME -> ACK -> ETX).
    display_rows = {
        0x50: "Varmvatten",
        0x51: "51.9 C",
        0x52: "Varmvattentemperatur",
        0x53: "1.0   14:25",
    }
    for cmd, text in display_rows.items():
        expect_ack(m, f"text poll {cmd:#04x}")
        master_sends(m, f"display row cmd {cmd:#04x}", cmd, text.encode("latin-1") + b"\x00")

    # 7. firmware's sim_actions kicks in ~45s after boot: curve offset write.
    #    The number control encodes -2 * factor(1) as 1 byte -> 0xFE.
    wait_enq_frame(m, "param write curve offset", frame(0xC0, ADDR_DISPLAY, bytes([0x00, 0x0B, 0xFE])), timeout=120)

    # 8. display button enter (0x04) via cmd 0x40
    wait_enq_frame(m, "button enter", frame(0x40, ADDR_DISPLAY, bytes([0x04, 0x85, 0x00, 0x00])), timeout=20)

    # 9. display button back (0x01)
    wait_enq_frame(m, "button back", frame(0x40, ADDR_DISPLAY, bytes([0x01, 0x85, 0x00, 0x00])), timeout=20)

    # 10. knob position 3 via cmd 0x40 (data 40 03 00 00)
    wait_enq_frame(m, "knob 3", frame(0x40, ADDR_DISPLAY, bytes([0x40, 0x03, 0x00, 0x00])), timeout=20)

    # 11. bad checksum frame -> node NAKs
    expect_ack(m, "csum poll")
    bad = frame(0xC0, ADDR_VVM_CPU, bytes.fromhex("00 01 01 f4"))
    bad = bad[:-1] + bytes([bad[-1] ^ 0xFF])  # corrupt checksum
    m.send(bad)
    nak = m.recv(1)[0]
    m.check("bad checksum -> NAK", nak == NAK, hex(nak))

    m.sock.close()
    print()
    if failures:
        print(f"FAILED: {failures} check(s) failed")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
