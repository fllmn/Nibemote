"""Nibe RS485 bus protocol (F-series / VVM indoor units).

Reverse engineered in the Swedish Elektronikforumet thread
"PC-styrning av Bergvärmepump" (https://elektronikforumet.com/forum/viewtopic.php?t=13714)
and cross-checked against the open-source implementations NibeGW (openHAB),
esphome-nibe and the Python `nibe` library (yozik04/nibe).

Physical layer: RS485, 19200 baud, 8N1, half duplex. The heat pump CPU board
is the bus master; accessories (display/RCU, RMU40, MODBUS40, SMS40, ...) are
slaves that must ACK when polled.

Frames on the wire:

  master -> slave : 5C <addr:u16be> <cmd> <len> <data...> <chk>
  slave  -> master: C0 <cmd> <len> <data...> <chk>
  single bytes    : 06 = ACK, 15 = NAK, 05 = ENQ, 03 = ETX

  * chk = XOR of all frame bytes after the start byte for master frames
    (5C excluded), and of all bytes *including* C0 for slave frames.
    If the XOR result equals 0x5C the transmitter sends 0xC5 instead
    (0x5C is the start byte and must not appear raw in a checksum).
  * 0x5C inside the data region is byte-stuffed as "5C 5C" (Dedupe5C);
    `len` counts the stuffed bytes.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field

START_MASTER = 0x5C  # heat pump -> accessory
START_SLAVE = 0xC0  # accessory -> heat pump
ACK = 0x06
NAK = 0x15
ENQ = 0x05
ETX = 0x03

MAX_DATA_LEN = 128  # per NibeGW; header + data + checksum must fit in 128+6


class Command(enum.IntEnum):
    RMU_WRITE_REQ = 0x60
    RMU_DATA_MSG = 0x62
    RMU_DATA_TOKEN = 0x63
    MODBUS_DATA_MSG = 0x68  # pump pushes register rows {addr:u16le, value:2B}*
    MODBUS_READ_REQ = 0x69  # accessory asks to read a register {addr:u16le}
    MODBUS_READ_RESP = 0x6A  # pump answers {addr:u16le, value:4B}
    MODBUS_WRITE_REQ = 0x6B  # accessory writes {addr:u16le, value:4B}
    MODBUS_WRITE_RESP = 0x6C  # pump answers {result:bool}
    PRODUCT_INFO_MSG = 0x6D
    MODBUS_ADDRESS_MSG = 0x6E
    ACCESSORY_TOKEN = 0xEE


class Address(enum.IntEnum):
    ECS_S2 = 0x02
    AXC40 = 0x05
    RCU = 0x14  # display / RCU panel (name from the original forum thread)
    SMS40 = 0x16
    RMU40_S1 = 0x19
    RMU40_S2 = 0x1A
    RMU40_S3 = 0x1B
    RMU40_S4 = 0x1C
    MODBUS40 = 0x20
    DEH500 = 0x27
    EME20 = 0xA4


def xor8(data: bytes) -> int:
    """XOR checksum with the 0x5C -> 0xC5 escape quirk."""
    chk = 0
    for b in data:
        chk ^= b
    return 0xC5 if chk == START_MASTER else chk


def stuff(data: bytes) -> bytes:
    """Escape 0x5C start bytes inside the data region (sent as 5C 5C)."""
    return data.replace(bytes([START_MASTER]), bytes([START_MASTER, START_MASTER]))


def unstuff(data: bytes) -> bytes:
    """Collapse stuffed 5C 5C pairs back to a single 0x5C."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b == START_MASTER and i + 1 < n and data[i + 1] == START_MASTER:
            out.append(b)
            i += 2
        else:
            out.append(b)
            i += 1
    return bytes(out)


def encode_master(address: int, cmd: int, data: bytes = b"") -> bytes:
    """Build a master frame: 5C <addr:u16be> <cmd> <len> <data> <chk>."""
    payload = stuff(data)
    if len(payload) > 0xFF:
        raise ValueError("data too long")
    body = bytes([(address >> 8) & 0xFF, address & 0xFF, cmd & 0xFF, len(payload)]) + payload
    return bytes([START_MASTER]) + body + bytes([xor8(body)])


def encode_slave(cmd: int, data: bytes = b"") -> bytes:
    """Build a slave frame: C0 <cmd> <len> <data> <chk> (chk covers C0 too)."""
    payload = stuff(data)
    if len(payload) > 0xFF:
        raise ValueError("data too long")
    body = bytes([cmd & 0xFF, len(payload)]) + payload
    return bytes([START_SLAVE]) + body + bytes([xor8(bytes([START_SLAVE]) + body)])


# ---------------------------------------------------------------------------
# Events produced by the stream parser
# ---------------------------------------------------------------------------


@dataclass
class MasterFrame:
    address: int
    cmd: int
    data: bytes  # already unstuffed
    raw: bytes
    checksum_ok: bool

    @property
    def command(self) -> Command | None:
        try:
            return Command(self.cmd)
        except ValueError:
            return None


@dataclass
class SlaveFrame:
    cmd: int
    data: bytes
    raw: bytes
    checksum_ok: bool

    @property
    def command(self) -> Command | None:
        try:
            return Command(self.cmd)
        except ValueError:
            return None


@dataclass
class ControlByte:
    """A single ACK/NAK/ENQ/ETX byte seen on the bus."""

    value: int

    @property
    def is_ack(self) -> bool:
        return self.value == ACK

    @property
    def is_nak(self) -> bool:
        return self.value == NAK


@dataclass
class Noise:
    """A byte that did not start a frame."""

    value: int


Event = MasterFrame | SlaveFrame | ControlByte | Noise


# ---------------------------------------------------------------------------
# Legacy protocol (Nibe Fighter 360P / VVM300 era) — the one from the forum
# thread t=13714, confirmed live on a VVM300 + F2025: plain 19200 8N1, 16-bit
# in-band addressing, no 9th bit needed on this hardware.
#
#   poll / address : <addr:u16be>                      (2 bytes)
#   node response  : 06 ACK | 05 ENQ (has data to send) | 15 NAK
#   data frame     : <cmd> <sender:u16be> <len> <data...> <csum>
#                    csum = XOR of cmd..last data byte (plain, no 0x5C quirk)
#   end of exchange: 03 ETX
#
# Verified against live capture, e.g. "55 00 0a 02 41 23 3f" (cmd 0x55,
# sender 0x000A=VVM300 CPU, len 2, data 41 23, csum 0x3f) and a 0x52 text frame
# "52 00 0a 15 <'Varmvattentemperatur\\0'> 70".
#
# Parameter frames use cmd 0xC0 with data = repeated groups
#   00 <index> <value:1-2 bytes>
# (FredRovers' spec in the forum thread.)
# ---------------------------------------------------------------------------

LEGACY_ACK = 0x06
LEGACY_NAK = 0x15
LEGACY_ENQ = 0x05
LEGACY_ETX = 0x03

LEGACY_CMD_PARAMS = 0xC0  # parameter dump / parameter write

# node addresses seen on a VVM300 + F2025 bus
LEGACY_ADDR_VVM_CPU = 0x000A  # sender address of VVM300 master frames
LEGACY_ADDR_RELAY = 0x00F5  # relay board
LEGACY_ADDR_DISPLAY = 0x00F9  # display card (what we impersonate)
LEGACY_ADDR_RCU = 0x0014  # RCU (external display, 360P-era)
LEGACY_ADDR_HEATPUMP_1 = 0x41C9  # F2025 heat pump


def xor8_plain(data: bytes) -> int:
    chk = 0
    for b in data:
        chk ^= b
    return chk


def encode_legacy_frame(cmd: int, sender: int, data: bytes = b"") -> bytes:
    """<cmd> <sender:u16be> <len> <data...> <xor8 csum>."""
    if len(data) > 0xFF:
        raise ValueError("data too long")
    body = bytes([cmd & 0xFF, (sender >> 8) & 0xFF, sender & 0xFF, len(data)]) + data
    return body + bytes([xor8_plain(body)])


@dataclass
class Poll:
    """A 2-byte address poll from the master."""

    address: int


@dataclass
class LegacyFrame:
    cmd: int
    sender: int
    data: bytes
    raw: bytes
    checksum_ok: bool


@dataclass
class Etx:
    pass


LegacyEvent = Poll | LegacyFrame | ControlByte | Etx


class LegacyParser:
    """State machine for the legacy VVM bus.

    exchange:  POLL(2B) -> RESP(1B) -> [FRAME -> RESP -> ETX]
    """

    _ADDR = 0
    _RESP1 = 1
    _FRAME_OR_ETX = 2  # after ACK: frame from master, or bare ETX
    _FRAME_HDR = 3
    _FRAME_BODY = 4
    _RESP2 = 5
    _ETX = 6
    _ENQ_ACK = 7  # after ENQ: expect master ACK, then frame from node
    _RESP2_SENT = 8  # we sent a frame ourselves; next bus byte is a new poll

    def note_sent_frame(self) -> None:
        """Call after transmitting a frame: the next received byte is the
        ACK/NAK to our frame, after which the bus returns to poll phase
        (we send the closing ETX ourselves, it is not echoed back)."""
        self._state = self._RESP2_SENT

    def note_self_response(self) -> None:
        """Call after WE answer a poll (ACK/ENQ): our own transmission is not
        echoed back to RX, so the parser must not wait for a response byte —
        the next received bytes are the master's frame or ETX."""
        if self._state == self._RESP1:
            self._state = self._FRAME_OR_ETX
        elif self._state == self._ENQ_ACK:
            self._state = self._FRAME_OR_ETX

    def __init__(self) -> None:
        self._state = self._ADDR
        self._buf = bytearray()
        self._addr = 0

    def feed(self, data: bytes) -> list[LegacyEvent]:
        events: list[LegacyEvent] = []
        for b in data:
            events.extend(self._feed_byte(b))
        return events

    def _feed_byte(self, b: int) -> list[LegacyEvent]:
        st = self._state
        if st == self._ADDR:
            self._buf.append(b)
            if len(self._buf) == 2:
                self._addr = (self._buf[0] << 8) | self._buf[1]
                self._buf.clear()
                self._state = self._RESP1
                return [Poll(self._addr)]
            return []
        if st == self._RESP1:
            if b in (LEGACY_ACK, LEGACY_ENQ, LEGACY_NAK):
                self._state = self._FRAME_OR_ETX if b == LEGACY_ACK else self._ENQ_ACK
                return [ControlByte(b)]
            if b in (0x00, 0x41):  # new poll started without a response -> resync
                self._state = self._ADDR
                self._buf = bytearray([b])
                return []
            self._state = self._ADDR
            self._buf.clear()
            return [Noise(b)]
        if st == self._ENQ_ACK:
            # master acknowledges our ENQ; next comes our frame
            self._state = self._FRAME_OR_ETX if b == LEGACY_ACK else self._ADDR
            if b in (LEGACY_ACK, LEGACY_NAK):
                return [ControlByte(b)]
            return []
        if st == self._FRAME_OR_ETX:
            if b == LEGACY_ETX:
                self._state = self._ADDR
                self._buf.clear()
                return [Etx()]
            if b in (0x00, 0x41):  # looks like a new poll's high byte -> resync
                self._state = self._ADDR
                self._buf = bytearray([b])
                return []
            self._buf = bytearray([b])
            self._state = self._FRAME_HDR
            return []
        if st == self._FRAME_HDR:
            self._buf.append(b)
            if len(self._buf) == 4:  # cmd + sender(2) + len
                self._state = self._FRAME_BODY
            return []
        if st == self._FRAME_BODY:
            self._buf.append(b)
            length = self._buf[3]
            if len(self._buf) == 4 + length + 1:
                raw = bytes(self._buf)
                self._buf.clear()
                self._state = self._RESP2
                return [
                    LegacyFrame(
                        cmd=raw[0],
                        sender=(raw[1] << 8) | raw[2],
                        data=raw[4:-1],
                        raw=raw,
                        checksum_ok=raw[-1] == xor8_plain(raw[:-1]),
                    )
                ]
            return []
        if st == self._RESP2:
            self._state = self._ETX
            if b in (LEGACY_ACK, LEGACY_NAK, LEGACY_ENQ):
                return [ControlByte(b)]
            self._state = self._ADDR
            return [Noise(b)]
        if st == self._RESP2_SENT:
            self._state = self._ADDR
            self._buf.clear()
            if b in (LEGACY_ACK, LEGACY_NAK, LEGACY_ENQ):
                return [ControlByte(b)]
            return [Noise(b)]
        if st == self._ETX:
            self._state = self._ADDR
            self._buf.clear()
            if b == LEGACY_ETX:
                return [Etx()]
            return [Noise(b)]
        return []


def parse_legacy_params(data: bytes) -> dict[int, bytes]:
    """Parse a 0xC0 parameter payload: repeated `00 <index> <value:1-2B>` groups.

    Value width is ambiguous on the wire; heuristic: a value is 2 bytes unless
    the next byte is a 0x00 group marker or the end of data.
    """
    params: dict[int, bytes] = {}
    i = 0
    n = len(data)
    while i < n:
        if data[i] != 0x00:
            i += 1
            continue
        if i + 1 >= n:
            break
        index = data[i + 1]
        j = i + 2
        if j < n and data[j] != 0x00:
            j += 1
            if j < n and data[j] != 0x00:
                j += 1
        params[index] = data[i + 2 : j]
        i = j
    return params


class FrameParser:
    """Incremental byte-stream parser for the Nibe RS485 bus.

    Feed bytes with `feed()`; it returns a list of parsed events. Both bus
    directions are handled since we sniff a shared half-duplex line.
    """

    _WAIT_START = 0
    _MASTER = 1
    _SLAVE = 2

    def __init__(self) -> None:
        self._state = self._WAIT_START
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[Event]:
        events: list[Event] = []
        for b in data:
            events.extend(self._feed_byte(b))
        return events

    def _feed_byte(self, b: int) -> list[Event]:
        if self._state == self._WAIT_START:
            if b == START_MASTER:
                self._state = self._MASTER
                self._buf = bytearray([b])
            elif b == START_SLAVE:
                self._state = self._SLAVE
                self._buf = bytearray([b])
            elif b in (ACK, NAK, ENQ, ETX):
                return [ControlByte(b)]
            else:
                return [Noise(b)]
            return []

        # inside a frame: header is start + 3 bytes (addr/cmd/len or cmd/len+1)
        self._buf.append(b)
        if self._state == self._MASTER:
            # need at least start + addr(2) + cmd + len = 5 bytes to know the length
            if len(self._buf) < 5:
                return []
            length = self._buf[4]
            if len(self._buf) < 5 + length + 1:
                return []
            raw = bytes(self._buf)
            self._state = self._WAIT_START
            body = raw[1:-1]
            ok = raw[-1] == xor8(body)
            return [
                MasterFrame(
                    address=(raw[1] << 8) | raw[2],
                    cmd=raw[3],
                    data=unstuff(raw[5:-1]),
                    raw=raw,
                    checksum_ok=ok,
                )
            ]
        else:  # slave
            if len(self._buf) < 3:
                return []
            length = self._buf[2]
            if len(self._buf) < 3 + length + 1:
                return []
            raw = bytes(self._buf)
            self._state = self._WAIT_START
            ok = raw[-1] == xor8(raw[:-1])  # checksum covers the C0 start byte
            return [
                SlaveFrame(
                    cmd=raw[1],
                    data=unstuff(raw[3:-1]),
                    raw=raw,
                    checksum_ok=ok,
                )
            ]


def parse_modbus_data_rows(data: bytes) -> dict[int, bytes]:
    """Parse a MODBUS_DATA_MSG payload: repeated {coil_address:u16le, value:2B}."""
    rows: dict[int, bytes] = {}
    for i in range(0, len(data) - 3, 4):
        addr = data[i] | (data[i + 1] << 8)
        rows[addr] = data[i + 2 : i + 4]
    return rows


def build_modbus_read_req(coil_address: int) -> bytes:
    """Payload for a MODBUS_READ_REQ slave frame (address is u16 little-endian)."""
    return bytes([coil_address & 0xFF, (coil_address >> 8) & 0xFF])


def build_modbus_write_req(coil_address: int, value: bytes) -> bytes:
    """Payload for a MODBUS_WRITE_REQ slave frame: addr u16le + 4-byte value."""
    if len(value) > 4:
        raise ValueError("value must fit in 4 bytes")
    return bytes([coil_address & 0xFF, (coil_address >> 8) & 0xFF]) + value.ljust(4, b"\x00")
