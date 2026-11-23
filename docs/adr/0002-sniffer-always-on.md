# The sniffer always runs; the emulator is a switch

Capture and decode are independent of whether the panel is transmitting, so the sniffer is
not a mode of the component but the way it always works. The two safety gates stay
independent booleans — the operator's arm switch for intent, the transport's transmit
capability for configuration — and the `Role` enum is dropped because "emulating" is
exactly `armed && transmit-capable`. The sniffer configuration keeps its transport passive,
so a misconfiguration cannot transmit.
