# An outbound command is a unit, registered once

Each command the panel can send is one unit holding its command byte, its payload, its
repeat rule and what it does with the reply; registration is one line. Adding a command is
therefore a task-side change that cannot touch the interrupt path. The four shipped
outbound behaviours — content request, key, knob, register read — become instances of the
same interface. Commands are code rather than YAML because a register write carries a
safety rule that must not be expressible by accident; ad-hoc experiments use the web
page's raw frame field instead of a declared command.
