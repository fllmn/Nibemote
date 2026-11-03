"""Daemon glue: bus + client + MQTT bridge, exposing bus parameters to HA."""

from __future__ import annotations

import logging
import time

from .bus import SerialBus
from .client import NibeClient, ParamUpdate
from .config import Settings
from .mqtt_bridge import MqttBridge
from .params import PARAMS

log = logging.getLogger(__name__)


class Daemon:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.bus = SerialBus(
            port=settings.serial_port,
            baud=settings.baud,
            parity=settings.parity,
            stopbits=settings.stopbits,
            rs485_rts=settings.rs485_rts,
        )
        self.client = NibeClient(
            self.bus,
            ack_addresses=[] if settings.passive else settings.addresses,
            on_param=self._on_param,
            on_event=self._on_event,
        )
        self.bridge = MqttBridge(
            host=settings.mqtt_host,
            port=settings.mqtt_port,
            topic_prefix=settings.mqtt_topic_prefix,
            ha_discovery_prefix=settings.ha_discovery_prefix,
            device_name=settings.device_name,
            device_id=settings.device_id,
            user=settings.mqtt_user,
            password=settings.mqtt_password,
            on_param_command=self._on_param_command,
            on_knob=self._on_knob,
            ha_discovery=settings.ha_discovery,
        )
        self._discovered: set[int] = set()

    def _on_event(self, ev) -> None:
        from . import protocol as p
        if isinstance(ev, p.Poll):
            log.info("POLL %#06x %s", ev.address,
                     "(ours -> ACK)" if ev.address in self.client.ack_addresses else "")
        elif isinstance(ev, p.LegacyFrame):
            log.info("FRAME cmd=%#04x from=%#06x chk=%s len=%d data=%s", ev.cmd, ev.sender,
                     ev.checksum_ok, len(ev.data), ev.data.hex(" ")[:100])
            if ev.checksum_ok and not self.settings.passive:
                self._publish_frame(ev)
        elif isinstance(ev, p.ControlByte):
            log.info("CTRL %#04x", ev.value)

    def _publish_frame(self, ev) -> None:
        """Expose every bus frame as an HA sensor: nibemote/frame_<cmd>_from_<sender>."""
        key = f"frame_{ev.cmd:02x}_from_{ev.sender:04x}"
        if key not in self._discovered:
            self._discovered.add(key)
            self.bridge.publish_generic_sensor(key, f"Bus frame {ev.cmd:#04x} from {ev.sender:#06x}")
        self.bridge.client.publish(
            f"{self.settings.mqtt_topic_prefix}/{key}/state",
            ev.data.hex(" "), retain=True)
        # decode the F2025 (0x41c9) cmd 0x65 sensor block: little-endian u16 pairs
        if ev.cmd == 0x65 and ev.sender == 0x41C9:
            for i in range(0, len(ev.data) - 1, 2):
                idx = i // 2
                val = int.from_bytes(ev.data[i : i + 2], "little", signed=True)
                dkey = f"f2025_blk65_{idx}"
                if dkey not in self._discovered:
                    self._discovered.add(dkey)
                    self.bridge.publish_generic_sensor(
                        dkey, f"F2025 0x65 block word {idx} (raw, x0.1 = {(val / 10):.1f})")
                self.bridge.client.publish(
                    f"{self.settings.mqtt_topic_prefix}/{dkey}/state", str(val), retain=True)

    def _on_param(self, update: ParamUpdate) -> None:
        param = PARAMS.get(update.index)
        try:
            value = param.decode(update.raw) if param else update.raw.hex()
        except Exception:
            value = update.raw.hex()
        name = param.name if param else f"param_{update.index:02x}"
        log.debug("param %#04x (%s) = %s", update.index, name, value)
        if update.index not in self._discovered:
            self._discovered.add(update.index)
            if param:
                self.bridge.publish_param_discovery(
                    update.index, name, f"{name} ({update.index:#04x})", param.unit,
                    param.writable, param.mappings,
                    is_bool=bool(param.mappings) and set(param.mappings) <= {0, 1},
                )
            else:
                self.bridge.publish_param_discovery(
                    update.index, name, f"{name} ({update.index:#04x})", "", False)
        self.bridge.publish_param_state(update.index, value)

    def _on_param_command(self, index: int, payload: str) -> None:
        param = PARAMS.get(index)
        if param is None or not param.writable:
            log.warning("param %#04x not writable", index)
            return
        raw = param.encode(payload)
        self.client.queue_write(index, raw)
        log.info("queued write param %#04x (%s) = %s -> %s", index, param.name,
                 payload, raw.hex(" "))

    def _on_knob(self, pos: int) -> None:
        """HA slider -> display knob frame: cmd 0x40, data [0x40, pos, 0, 0]."""
        pos &= 0xFF
        self.client.queue_frame(0x40, bytes([0x40, pos, 0x00, 0x00]))
        self.bridge.publish_knob_state(pos)
        log.info("knob -> %#04x (curve offset adjust)", pos)

    def run(self) -> None:
        self.bus.open()
        # emulate the display's boot announcement (captured on the real panel)
        if not self.settings.passive:
            self.client.queue_announce(0x40, bytes.fromhex("40 85 00 00"))
        mqtt_ok = False
        try:
            self.bridge.connect()
            mqtt_ok = True
        except Exception:
            log.warning("MQTT connect failed; running bus-only, will retry", exc_info=True)
        if not self.settings.passive:
            self.bridge.publish_knob_discovery()
        if self.settings.passive:
            log.info("Daemon running in PASSIVE listen mode (never transmits)")
        else:
            log.info("Daemon running; answering polls for %s",
                     ", ".join(hex(a) for a in sorted(self.settings.addresses)))
        last_mqtt_retry = 0.0
        try:
            while True:
                self.client.poll()
                if not mqtt_ok and time.monotonic() - last_mqtt_retry > 15.0:
                    last_mqtt_retry = time.monotonic()
                    try:
                        self.bridge.connect()
                        mqtt_ok = True
                        log.info("MQTT connected")
                    except Exception:
                        log.warning("MQTT still unavailable")
                time.sleep(0.005)
        except KeyboardInterrupt:
            log.info("shutting down")
        finally:
            self.client.stop()
            if mqtt_ok:
                self.bridge.disconnect()
            self.bus.close()
