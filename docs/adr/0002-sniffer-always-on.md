# The sniffer always runs; the emulator is a switch

Capture and decode are independent of whether the panel is transmitting, so the sniffer is
not a mode of the component but the way it always works. The two safety gates stay
independent booleans — the operator's arm switch for intent, the transport's transmit
capability for configuration — and the `Role` enum is dropped because "emulating" is
exactly `armed && transmit-capable`. The config's `role:` value is not a third gate: it
records what the build intends to be, which is what enables the switchless host arming and
the loud refusal when a `role: emulator` build has no transmit-capable transport.
The sniffer configuration keeps its transport passive, so a misconfiguration cannot
transmit.
