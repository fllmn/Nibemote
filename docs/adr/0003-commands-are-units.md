# An outbound command is a unit, registered once

Each command the panel initiates is one unit holding its command byte, its payload, its
repeat rule and what it does with the reply; registration is one line, and the registry is
the outbound command surface shown in `dump_config`. Adding a command is therefore a
task-side change that cannot touch the interrupt path. The four shipped outbound
behaviours — content request, key, knob, register read — are instances of the same
interface. Answers to master questions (the 0x62 menu reply, the 0xD4 clock) are not
queries: they belong to the responder, because the master asked. Commands are code rather
than YAML because a register write carries a safety rule that must not be expressible by
accident; ad-hoc experiments use the web page's raw frame field instead of a declared
command.
