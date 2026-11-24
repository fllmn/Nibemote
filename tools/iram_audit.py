#!/usr/bin/env python3
"""Fail if an ISR-reachable symbol lives in flash.

The bus ISR must keep answering while OTA writes disable the flash cache. A
function it calls that is not in IRAM faults with "Cache error" and reboots the
device in the middle of the upload — the failure recorded in docs/bring-up.md.

This checks the built image rather than the source:

- a symbol with no address was inlined; its code sits inside an IRAM caller, so
  it is safe;
- a symbol with an address must be in the ESP32-S3 IRAM range.

The list below is the ISR surface. A new function the ISR can reach means a new
entry here — deliberate, so the surface stays visible in review. Include static
helpers and templates by their demangled prefix; c++filt is used to match them.
"""
import subprocess
import sys

IRAM_LO, IRAM_HI = 0x40370000, 0x403E0000  # ESP32-S3 IRAM

ISR_SYMBOLS = [
    # nibe engine: ISR entry points, wire mechanics, emission
    "esphome::nibe::NibeBus::rx_byte_trampoline_",
    "esphome::nibe::NibeBus::gap_trampoline_",
    "esphome::nibe::NibeBus::tx_done_trampoline_",
    "esphome::nibe::NibeBus::rx_byte_",
    "esphome::nibe::NibeBus::gap_",
    "esphome::nibe::NibeBus::track_byte_",
    "esphome::nibe::NibeBus::push_wire_",
    "esphome::nibe::NibeBus::push_wire_gap_",
    "esphome::nibe::NibeBus::push_tx_notice_",
    "esphome::nibe::NibeBus::reply_to_poll_",
    "esphome::nibe::NibeBus::deliver_frame_",
    "esphome::nibe::NibeBus::feed_paced_",
    "esphome::nibe::NibeBus::arm_reply_",
    "esphome::nibe::NibeBus::fire_pending_reply_",
    "esphome::nibe::NibeBus::purge_stale_requests_",
    "esphome::nibe::NibeBus::poll_reply_tick_",
    "esphome::nibe::ScreenState::any",
    # predicates and capability checks: inlined today, IRAM if ever emitted
    "esphome::nibe::NibeBus::emulating",
    "esphome::nibe::NibeBus::transmit_capable_",
    "esphome::nibe::is_poll_hi",
    "esphome::nibe::is_ctrl_byte",
    "esphome::nibe::q_load_",
    "esphome::nibe::q_store_",
    "esphome::nibe::ring_push_",
    # transport: the hardware ISR and everything it touches
    "esphome::rs485_bus::RS485Bus::uart_isr_",
    "esphome::rs485_bus::RS485Bus::en_set_level_",
    "esphome::rs485_bus::RS485Bus::send_burst",
    "esphome::rs485_bus::RS485Bus::ring_push_",
    "esphome::rs485_bus::RS485Bus::now_us_",
    "esphome::rs485_bus::RS485Bus::bus_active",
    "esphome::rs485_bus::RS485Bus::rx_silence_us",
]


def func_symbols(elf):
    """[(address, demangled_name)] for every defined function symbol."""
    out = subprocess.run(["readelf", "-sW", elf], capture_output=True, text=True, check=True).stdout
    raw = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[1] != "0":
            raw.append((int(p[1], 16), p[7]))
    if not raw:
        sys.exit(f"iram-audit: no function symbols in {elf} — build the device image first")
    demangled = subprocess.run(
        ["c++filt"], input="\n".join(n for _, n in raw), capture_output=True, text=True, check=True
    ).stdout.splitlines()
    return [(addr, dem) for (addr, _), dem in zip(raw, demangled)]


def matches(sym, entry):
    return sym == entry or sym.startswith(entry + "(") or sym.startswith(entry + "<")


def main():
    elf = "configs/.esphome/build/nibemote/build/firmware.elf"
    entries = list(ISR_SYMBOLS)
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == "--also":
            if i + 1 >= len(argv):
                sys.exit("iram-audit: --also needs a demangled symbol prefix")
            entries.append(argv[i + 1])
            i += 2
        else:
            elf = argv[i]
            i += 1

    symbols = func_symbols(elf)
    failures = []
    checked = inlined = 0
    for entry in entries:
        hits = [(addr, dem) for addr, dem in symbols if matches(dem, entry)]
        if not hits:
            inlined += 1
            continue
        for addr, dem in hits:
            checked += 1
            if not IRAM_LO <= addr < IRAM_HI:
                failures.append((addr, dem))

    if failures:
        print(f"iram-audit: FAIL — {len(failures)} ISR-reachable symbol(s) in flash:")
        for addr, dem in failures:
            print(f"  0x{addr:08x}  {dem}")
        print("Add IRAM_ATTR (the ISR must run while OTA disables the flash cache).")
        return 1

    print(f"iram-audit: ok — {checked} ISR symbol(s) in IRAM, {inlined} inlined")
    return 0


if __name__ == "__main__":
    sys.exit(main())
