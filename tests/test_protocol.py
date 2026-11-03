"""Protocol codec tests.

Vectors are taken from real bus captures / shipped implementations:
  * NibePi backend.js sendQueue frames (slave REQUEST frames captured from a
    working VVM installation):
      C0 6B 06 73 B0 01 00 00 00 6F
      C0 6B 06 D4 BE 00 00 00 00 C7
  * checksum quirk: xor == 0x5C must be transmitted as 0xC5 (NibeGW/yozik04-nibe)
"""

from nibemote import protocol as p


def test_slave_frame_checksum_includes_start_byte():
    # captured NibePi write frame: C0 6B 06 73 B0 01 00 00 00 6F
    raw = bytes.fromhex("C0 6B 06 73 B0 01 00 00 00 6F")
    assert p.xor8(raw[:-1]) == raw[-1] == 0x6F
    # and NOT the xor without the C0:
    assert p.xor8(raw[1:-1]) != raw[-1]


def test_encode_slave_matches_captured_frame():
    frame = p.encode_slave(0x6B, bytes.fromhex("73 B0 01 00 00 00"))
    assert frame == bytes.fromhex("C0 6B 06 73 B0 01 00 00 00 6F")
    frame2 = p.encode_slave(0x6B, bytes.fromhex("D4 BE 00 00 00 00"))
    assert frame2 == bytes.fromhex("C0 6B 06 D4 BE 00 00 00 00 C7")


def test_checksum_quirk_5c_becomes_c5():
    # find data whose xor over (C0 + body) is 0x5C: C0 ^ cmd ^ len ^ data == 5C
    # choose cmd=0x00, len=0x01, data d: C0^00^01^d = 5C -> d = C0^00^01^5C = 9D
    frame = p.encode_slave(0x00, b"\x9d")
    assert frame[-1] == 0xC5
    assert p.xor8(frame[:-1]) == 0xC5  # decodes with quirk applied


def test_master_frame_layout_and_checksum_excludes_start():
    frame = p.encode_master(0x20, 0x68, b"\x01\x02")
    assert frame[0] == 0x5C
    assert frame[1:3] == b"\x00\x20"  # address big-endian
    assert frame[3] == 0x68
    assert frame[4] == 2
    assert frame[5:7] == b"\x01\x02"
    assert frame[-1] == p.xor8(frame[1:-1])


def test_stuffing_roundtrip():
    data = bytes([0x5C, 0x00, 0x5C, 0x5C, 0x41])
    stuffed = p.stuff(data)
    assert stuffed == bytes([0x5C, 0x5C, 0x00, 0x5C, 0x5C, 0x5C, 0x5C, 0x41])
    assert p.unstuff(stuffed) == data
    frame = p.encode_master(0x14, 0x51, data)
    assert frame[4] == len(stuffed)  # len counts stuffed bytes
    parser = p.FrameParser()
    (ev,) = parser.feed(frame)
    assert isinstance(ev, p.MasterFrame)
    assert ev.checksum_ok
    assert ev.data == data  # unstuffed on parse
    assert ev.address == 0x14


def test_parser_handles_ack_then_frame_and_noise():
    parser = p.FrameParser()
    stream = b"\x06" + p.encode_master(0x14, 0x69, b"") + b"\x99\x15"
    events = parser.feed(stream)
    kinds = [type(e) for e in events]
    assert kinds == [p.ControlByte, p.MasterFrame, p.Noise, p.ControlByte]
    assert events[0].is_ack
    assert events[1].command == p.Command.MODBUS_READ_REQ
    assert events[3].is_nak


def test_parser_slave_frame():
    parser = p.FrameParser()
    events = parser.feed(bytes.fromhex("C0 6B 06 73 B0 01 00 00 00 6F"))
    (ev,) = events
    assert isinstance(ev, p.SlaveFrame)
    assert ev.cmd == 0x6B
    assert ev.data == bytes.fromhex("73 B0 01 00 00 00")
    assert ev.checksum_ok


def test_parser_split_across_feeds():
    parser = p.FrameParser()
    frame = p.encode_master(0x20, 0x6A, b"\x01\x02\x03\x04\x05\x06")
    events = []
    for b in frame:
        events.extend(parser.feed(bytes([b])))
    assert len(events) == 1
    assert events[0].checksum_ok
    assert events[0].data == b"\x01\x02\x03\x04\x05\x06"


def test_bad_checksum_detected():
    parser = p.FrameParser()
    frame = bytearray(p.encode_master(0x20, 0x68, b"\x01\x02"))
    frame[-1] ^= 0xFF
    (ev,) = parser.feed(bytes(frame))
    assert isinstance(ev, p.MasterFrame)
    assert not ev.checksum_ok


def test_modbus_data_rows():
    # two rows: addr 40004 (0x9C44) -> 0x00FA, addr 40013 (0x9C4D) -> 0x0145
    data = bytes.fromhex("44 9C FA 00 4D 9C 45 01")
    rows = p.parse_modbus_data_rows(data)
    assert rows == {40004: b"\xfa\x00", 40013: b"\x45\x01"}


def test_build_requests_little_endian():
    assert p.build_modbus_read_req(40004) == bytes([0x44, 0x9C])
    assert p.build_modbus_write_req(40004, b"\x45\x01") == bytes.fromhex("44 9C 45 01 00 00")
