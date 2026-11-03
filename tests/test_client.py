"""Client tests against a scripted fake legacy bus (no hardware)."""

import time

from nibemote import protocol as p
from nibemote.client import NibeClient


class FakeBus:
    """Emulates the VVM300 master: polls a node, answers our frames."""

    def __init__(self, poll_address=0x00F9, respond=True):
        self.is_open = True
        self.sent: list[bytes] = []
        self.inbox = bytearray()
        self.poll_address = poll_address
        self.respond = respond
        self._polled = False

    def read(self, max_bytes=4096):
        if not self._polled:
            self._polled = True
            self.inbox += bytes([self.poll_address >> 8, self.poll_address & 0xFF])
        data = bytes(self.inbox)
        self.inbox.clear()
        return data

    def write(self, data: bytes):
        self.sent.append(data)
        if not self.respond:
            return
        if len(data) == 1 and data[0] in (p.LEGACY_ACK, p.LEGACY_NAK, p.LEGACY_ENQ):
            if data[0] == p.LEGACY_ENQ:
                self.inbox += bytes([p.LEGACY_ACK])  # master accepts our ENQ
            return
        # otherwise try to parse a data frame (feed with a preceding poll+ack so
        # the parser state machine is aligned)
        events = p.LegacyParser().feed(bytes([0x00, 0x0A, p.LEGACY_ACK]) + data)
        for ev in events:
            if isinstance(ev, p.LegacyFrame):
                if ev.checksum_ok:
                    self.inbox += bytes([p.LEGACY_ACK, p.LEGACY_ETX])
                else:
                    self.inbox += bytes([p.LEGACY_NAK, p.LEGACY_ETX])


def test_ack_poll_for_our_address():
    bus = FakeBus()
    client = NibeClient(bus, ack_addresses=[0x00F9])
    client.poll()
    assert b"\x06" in b"".join(bus.sent)
    assert client.stats["polls"] == 1 and client.stats["acks_sent"] == 1


def test_ignore_poll_for_other_address():
    bus = FakeBus(poll_address=0x00F5)  # relay board, not ours
    client = NibeClient(bus, ack_addresses=[0x00F9])
    client.poll()
    assert bus.sent == []


def test_param_frame_parsed_and_acked():
    class ParamBus(FakeBus):
        def read(self, max_bytes=4096):
            self.inbox += bytes([0x00, 0xF9])  # poll us
            return bytes(self.inbox) or super().read()

    # simpler: craft poll + param frame directly
    bus = FakeBus(respond=False)
    client = NibeClient(bus, ack_addresses=[0x00F9])
    updates = []
    client.on_param = updates.append
    # master polls us, then sends a 0xC0 frame with params 04=0x0125, 05=0x0110
    payload = bytes([0x00, 0x04, 0x01, 0x25, 0x00, 0x05, 0x01, 0x10])
    stream = bytes([0x00, 0xF9]) + b"\x06" + p.encode_legacy_frame(0xC0, 0x000A, payload) + b"\x06\x03"
    events = client.parser.feed(stream)
    for ev in events:
        client._dispatch(ev)
    assert client.params[0x04] == b"\x01\x25"
    assert client.params[0x05] == b"\x01\x10"
    assert [u.index for u in updates] == [0x04, 0x05]
    # we ACKed the poll and the frame
    sent = b"".join(bus.sent)
    assert sent.count(b"\x06") == 2


def test_write_flow_enq_then_frame():
    bus = FakeBus()
    client = NibeClient(bus, ack_addresses=[0x00F9])
    client.queue_write(0x0B, b"\x05")
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        client.poll()
        time.sleep(0.005)
        if not client._write_queue and client._pending_write is None:
            break
    sent = b"".join(bus.sent)
    # we answered ENQ, then sent the 0xC0 write frame with 00 0B 05
    assert b"\x05" in sent
    expected = p.encode_legacy_frame(0xC0, 0x00F9, bytes([0x00, 0x0B, 0x05]))
    assert expected in sent


def test_legacy_codec_vectors_from_live_capture():
    # captured on a real VVM300: cmd 0x55 sender 0x000A len 2 data 41 23 csum 3f
    assert p.encode_legacy_frame(0x55, 0x000A, bytes.fromhex("41 23")) == bytes.fromhex(
        "55 00 0a 02 41 23 3f")
    # text frame: cmd 0x52, sender 0x000A, len 0x15, "Varmvattentemperatur\0", csum 0x70
    text = b"Varmvattentemperatur\x00"
    assert p.encode_legacy_frame(0x52, 0x000A, text) == bytes.fromhex(
        "52 00 0a 15") + text + bytes([0x70])


def test_parse_legacy_params():
    payload = bytes([0x00, 0x04, 0x01, 0x25, 0x00, 0x05, 0x01, 0x10, 0x00, 0x0A, 0x07])
    params = p.parse_legacy_params(payload)
    assert params == {0x04: b"\x01\x25", 0x05: b"\x01\x10", 0x0A: b"\x07"}


def test_announce_flow_matches_boot_capture():
    # boot capture: poll 00 f9 -> ENQ 05 -> master 06 -> frame 40 00 f9 04 40 85 00 00 78 -> 06 -> 03
    class BootBus(FakeBus):
        def write(self, data: bytes):
            self.sent.append(data)
            if len(data) == 1 and data[0] == p.LEGACY_ENQ:
                self.inbox += bytes([p.LEGACY_ACK])
                return
            if len(data) == 1:
                return
            events = p.LegacyParser().feed(bytes([0x00, 0x0A, p.LEGACY_ACK]) + data)
            for ev in events:
                if isinstance(ev, p.LegacyFrame) and ev.checksum_ok:
                    self.inbox += bytes([p.LEGACY_ACK])
    bus = BootBus()
    client = NibeClient(bus, ack_addresses=[0x00F9])
    client.queue_announce(0x40, bytes.fromhex("40 85 00 00"))
    import time as _t
    deadline = _t.monotonic() + 2.0
    while _t.monotonic() < deadline:
        client.poll()
        _t.sleep(0.005)
        if not client._await_frame_ack and not client._await_master_ack:
            break
    sent = b"".join(bus.sent)
    expected = p.encode_legacy_frame(0x40, 0x00F9, bytes.fromhex("40 85 00 00"))
    assert expected in sent                      # the exact boot frame from the capture
    assert expected == bytes.fromhex("40 00 f9 04 40 85 00 00 78")
    assert sent.endswith(b"\x03")                # we closed with ETX after the ACK


def test_boot_handshake_22_then_62():
    bus = FakeBus()
    client = NibeClient(bus, ack_addresses=[0x00F9])
    # master polls us, then sends boot ping 0x22, then menu request 0x62
    client._dispatch(p.Poll(0x00F9))
    client._dispatch(p.LegacyFrame(cmd=0x22, sender=0x000A, data=b"", raw=b"", checksum_ok=True))
    assert client._boot_announces == 3  # 0x22 -> announce 0x40 on next 3 polls
    client._dispatch(p.LegacyFrame(cmd=0x62, sender=0x000A, data=b"", raw=b"", checksum_ok=True))
    deadline = time.monotonic() + 1.5
    sent = b""
    while time.monotonic() < deadline:
        client.poll()
        time.sleep(0.005)
        sent = b"".join(bus.sent)
        if p.encode_legacy_frame(0x62, 0x00F9, b"\x0f") in sent:
            break
    assert p.encode_legacy_frame(0x62, 0x00F9, b"\x0f") in sent  # replied 62 0f
