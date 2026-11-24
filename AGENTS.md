# nibemote2 — agent notes

## Commands
- Toolchain is repo-local, uv-managed: pyproject.toml declares esphome, uv.lock pins the resolution, .python-version pins 3.13; `make env` (= `uv sync`) provisions .venv; binary at `.venv/bin/esphome`.
  - `make gate` — the pre-commit verification: validate + compile every config, IRAM-audit the device image, then the host build against the sim (all checks must pass).
  - `make flash-emulator` / `make flash-sniffer` — network OTA to the device (`DEVICE=10.42.0.131` by default; `CONFIG=`/`DEVICE=` override).
  - `make logs` — tail the device; `make sim` — the sim gate alone; `make distclean` — drop builds + .venv.
  - Raw esphome works too: `.venv/bin/esphome config|compile|upload configs/nibemote.yaml`.
- Verify before committing: `make gate`.

## Conventions
- This repo is safety-critical (heating). Never enable TX paths by default; `rs485_bus: active: true` is
  opt-in per config. The sniffer config must always be passive.
- The bus protocol is Nibe legacy framing (NOT Modbus RTU). Docs are split by content, not by evidence
  status: docs/protocol.md = wire behavior, one page, facts only (no capture/provenance citations, no
  negations); docs/commands.md = commands, keys and registers; docs/hypotheses.md = open items with
  their evidence and how to settle them. Keep hypotheses OUT of protocol.md/commands.md; the prior
  ../nibemote repo must be cited as evidence, not trusted as code.
- ISR path rules (rs485_bus): IRAM_ATTR code only, no virtual dispatch, DRAM data only, no logging,
  no allocation. Everything else happens at loop/task level via event rings.
