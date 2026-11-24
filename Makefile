# nibemote2 — Nibe VVM300/F2025 display replacement (ESPHome firmware)
#
# The esphome toolchain lives in this repo, uv-managed: pyproject.toml declares the
# dependency, uv.lock pins the exact resolution, .python-version pins the
# interpreter. `make env` runs `uv sync` to provision .venv from the lock.

ESPHOME := .venv/bin/esphome
PY      := .venv/bin/python

DEVICE  ?= 10.42.0.131             # the Waveshare ESP32-S3 on the bus (network OTA)
CONFIG  ?= configs/nibemote.yaml  # default flash/logs target: the display emulator

DEVICE_CONFIGS := nibemote nibemote-sniffer
ALL_CONFIGS    := $(DEVICE_CONFIGS) nibemote-host

.DEFAULT_GOAL := help
.PHONY: help env validate compile sim gate flash flash-emulator flash-sniffer logs clean distclean

help: ## list targets
	@grep -hE '^[a-z-]+:.*?##' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-16s %s\n",$$1,$$2}'

env: ## provision .venv from pyproject/uv.lock (uv sync)
	uv sync

validate: env ## validate every config yaml
	@for c in $(ALL_CONFIGS); do echo "== validate $$c"; \
	  $(ESPHOME) config configs/$$c.yaml >/dev/null || exit 1; done
	@echo "all configs valid"

compile: env ## compile both device builds + the host build
	@for c in $(ALL_CONFIGS); do echo "== compile $$c"; \
	  $(ESPHOME) compile configs/$$c.yaml >/dev/null || exit 1; done
	@echo "all configs compiled"

test: ## plain-C++ tests for the pure decode/encode helpers
	@$(CXX) -std=c++20 -Wall -Wextra -Werror -O1 -Icomponents/nibe tests/test_decode.cpp components/nibe/nibe_decode.cpp -o /tmp/nibemote-decode-test
	@/tmp/nibemote-decode-test

iram-audit: ## fail if an ISR-path symbol sits in flash (after a device build)
	@$(PY) tools/iram_audit.py configs/.esphome/build/nibemote/build/firmware.elf

sim: env ## run the host build against sim/vvm300_sim.py (all checks must pass)
	@pat='[c]onfigs/.esphome/build/nibemote-host/.pioenvs/nibemote-host/program'; \
	  pkill -f "$$pat" 2>/dev/null || true; \
	  $(ESPHOME) run configs/nibemote-host.yaml > /tmp/nibemote-host-run.log 2>&1 & \
	  host=$$!; trap 'kill $$host 2>/dev/null; pkill -f "$$pat" 2>/dev/null' EXIT; \
	  ok=0; for i in $$(seq 1 60); do \
	    $(PY) -c "import socket; socket.create_connection(('127.0.0.1', 6638), 0.25).close()" \
	      2>/dev/null && { ok=1; break; }; sleep 1; \
	  done; \
	  if [ $$ok -ne 1 ]; then echo "host firmware never opened tcp_uart 6638 (see /tmp/nibemote-host-run.log)" >&2; exit 1; fi; \
	  $(PY) sim/vvm300_sim.py; rc=$$?; kill $$host 2>/dev/null; pkill -f "$$pat" 2>/dev/null; exit $$rc

gate: validate compile iram-audit test sim ## pre-commit gate (AGENTS.md: validate + compile + IRAM audit + decode tests + host sim)

flash: env ## OTA-flash CONFIG to DEVICE (default: display emulator)
	$(ESPHOME) upload --device $(DEVICE) $(CONFIG)

flash-emulator: CONFIG = configs/nibemote.yaml
flash-sniffer: CONFIG = configs/nibemote-sniffer.yaml
flash-emulator: flash ## flash the display emulator (nibemote.yaml)
flash-sniffer: flash ## flash the passive sniffer (nibemote-sniffer.yaml)

logs: env ## tail live logs of DEVICE (Ctrl-C to stop)
	$(ESPHOME) logs --device $(DEVICE) $(CONFIG)

clean: env ## remove esphome build directories
	@for c in $(ALL_CONFIGS); do $(ESPHOME) clean configs/$$c.yaml; done

distclean: clean ## clean + drop .venv
	rm -rf .venv
