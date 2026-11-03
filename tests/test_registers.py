from nibemote.registers import load_registers


def test_map_loads():
    regs = load_registers()
    assert len(regs) > 800
    assert 40004 in regs
    bt1 = regs[40004]
    assert bt1.title.startswith("BT1")
    assert bt1.unit == "°C"
    assert bt1.size == "s16"
    assert bt1.factor == 10


def test_decode_s16_factor10():
    regs = load_registers()
    bt1 = regs[40004]
    # 0x0125 = 293 -> 29.3 °C (the example from the forum thread)
    assert bt1.decode(bytes.fromhex("25 01")) == 29.3


def test_decode_negative():
    regs = load_registers()
    bt1 = regs[40004]
    # -55 raw -> -5.5 °C  (0xFFC9 little endian)
    assert bt1.decode(bytes.fromhex("C9 FF")) == -5.5


def test_encode_roundtrip():
    regs = load_registers()
    bt1 = regs[40004]
    raw = bt1.encode(29.3)
    assert raw == bytes.fromhex("25 01")
    assert bt1.decode(raw) == 29.3


def test_enum_mappings():
    regs = load_registers()
    enums = [r for r in regs.values() if r.mappings]
    assert enums, "expected some enum registers"
    reg = next(r for r in enums if r.writable)
    label = next(iter(reg.mappings.values()))
    raw = reg.encode(label)
    assert reg.decode(raw) == label


def test_word_swap_32bit():
    regs = load_registers()
    reg = next(r for r in regs.values() if r.size in ("u32", "s32") and r.factor == 1)
    raw = reg.encode(0x01020304)
    # little endian without swap
    assert raw == bytes.fromhex("04 03 02 01")
    swapped = reg.encode(0x01020304, word_swap=True)
    assert swapped == bytes.fromhex("02 01 04 03")
    assert reg.decode(swapped, word_swap=True) == 0x01020304


def test_writable_count():
    regs = load_registers()
    writable = [r for r in regs.values() if r.writable]
    assert len(writable) > 400
