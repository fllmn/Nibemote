"""MQTT bridge with Home Assistant MQTT discovery.

Every register in the map becomes an HA entity:
  * writable + mappings            -> select   (state + command topic)
  * writable + 0..1                -> switch
  * writable numeric with min/max  -> number
  * writable numeric without range -> number (no min/max)
  * read-only                      -> sensor
State is published retained to <prefix>/<register>/state; commands arrive on
<prefix>/<register>/set. Discovery configs go to <ha_prefix>/<component>/.../config.
"""

from __future__ import annotations

import json
import logging
from typing import Callable

import paho.mqtt.client as mqtt

from .registers import Register

log = logging.getLogger(__name__)

_UNIT_DEVICE_CLASS = {
    "°C": ("temperature", "measurement"),
    "kWh": ("energy", "total_increasing"),
    "Wh": ("energy", "total_increasing"),
    "kW": ("power", "measurement"),
    "W": ("power", "measurement"),
    "V": ("voltage", "measurement"),
    "A": ("current", "measurement"),
    "h": ("duration", "total_increasing"),
    "hrs": ("duration", "total_increasing"),
    "min": ("duration", "measurement"),
    "secs": ("duration", "measurement"),
    "bar": ("pressure", "measurement"),
    "kPa": ("pressure", "measurement"),
    "%": (None, "measurement"),
    "%RH": ("humidity", "measurement"),
    "Hz": ("frequency", "measurement"),
    "rpm": (None, "measurement"),
    "l/m": ("volume_flow_rate", "measurement"),
}


class MqttBridge:
    def __init__(
        self,
        host: str,
        port: int,
        topic_prefix: str,
        ha_discovery_prefix: str,
        device_name: str,
        device_id: str,
        user: str = "",
        password: str = "",
        on_command: Callable[[int, str], None] | None = None,
        on_param_command: Callable[[int, str], None] | None = None,
        on_knob: Callable[[int], None] | None = None,
        ha_discovery: bool = True,
    ) -> None:
        self.topic_prefix = topic_prefix
        self.ha_prefix = ha_discovery_prefix
        self.device_name = device_name
        self.device_id = device_id
        self.on_command = on_command
        self.on_param_command = on_param_command
        self.on_knob = on_knob
        self.ha_discovery = ha_discovery
        self._by_topic: dict[str, int] = {}
        self._param_topics: dict[str, int] = {}
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=device_id)
        if user:
            self.client.username_pw_set(user, password or None)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self._host = host
        self._port = port

    # -- lifecycle ---------------------------------------------------------

    def connect(self) -> None:
        self.client.connect(self._host, self._port, keepalive=60)
        self.client.loop_start()
        log.info("MQTT connected to %s:%s", self._host, self._port)

    def disconnect(self) -> None:
        self.client.publish(f"{self.topic_prefix}/availability", "offline", retain=True)
        self.client.loop_stop()
        self.client.disconnect()

    # -- discovery ----------------------------------------------------------

    def publish_discovery(self, registers: dict[int, Register]) -> None:
        for reg in registers.values():
            if self.ha_discovery:
                self._publish_config(reg)
            self._by_topic[f"{self.topic_prefix}/{reg.name}/set"] = reg.address
        self.client.publish(f"{self.topic_prefix}/availability", "online", retain=True)

    def _device(self) -> dict:
        return {
            "identifiers": [self.device_id],
            "name": self.device_name,
            "manufacturer": "Nibe (reverse engineered)",
            "model": "F2025 + VVM300 via RS485",
        }

    def _publish_config(self, reg: Register) -> None:
        topic_base = f"{self.topic_prefix}/{reg.name}"
        state_topic = f"{topic_base}/state"
        common = {
            "name": reg.title,
            "unique_id": f"{self.device_id}_{reg.name}",
            "object_id": f"{self.device_id}_{reg.name}",
            "device": self._device(),
            "state_topic": state_topic,
            "availability_topic": f"{self.topic_prefix}/availability",
            "json_attributes_topic": f"{topic_base}/attributes",
        }
        if reg.unit:
            common["unit_of_measurement"] = reg.unit
        cfg: dict
        if reg.writable and reg.is_enum:
            cfg = {
                **common,
                "command_topic": f"{topic_base}/set",
                "options": sorted(reg.mappings.values()),
            }
            component = "select"
        elif reg.writable and reg.is_boolean:
            cfg = {**common, "command_topic": f"{topic_base}/set", "payload_on": "1", "payload_off": "0"}
            component = "switch"
        elif reg.writable:
            cfg = {**common, "command_topic": f"{topic_base}/set"}
            if reg.min is not None:
                cfg["min"] = reg.min
            if reg.max is not None:
                cfg["max"] = reg.max
            if reg.factor and reg.factor != 1:
                cfg["step"] = round(1 / reg.factor, 6)
            component = "number"
        else:
            dev_class, state_class = _UNIT_DEVICE_CLASS.get(reg.unit, (None, "measurement"))
            cfg = {**common}
            if dev_class:
                cfg["device_class"] = dev_class
            if state_class:
                cfg["state_class"] = state_class
            component = "sensor"
        cfg["icon"] = cfg.get("icon", "mdi:heat-pump")
        topic = f"{self.ha_prefix}/{component}/{self.device_id}/{reg.name}/config"
        self.client.publish(topic, json.dumps(cfg), retain=True)

    # -- state ---------------------------------------------------------------

    def publish_state(self, reg: Register, value) -> None:
        topic = f"{self.topic_prefix}/{reg.name}/state"
        payload = value if isinstance(value, str) else json.dumps(value)
        self.client.publish(topic, payload, retain=True)
        attrs = {"address": reg.address, "title": reg.title}
        if reg.info:
            attrs["info"] = reg.info
        self.client.publish(
            f"{self.topic_prefix}/{reg.name}/attributes", json.dumps(attrs), retain=True
        )

    # -- legacy parameter entities -------------------------------------------

    def publish_generic_sensor(self, key: str, title: str, unit: str = "") -> None:
        """Publish a read-only sensor discovery + register nothing writable."""
        base = f"{self.topic_prefix}/{key}"
        common = {
            "name": title,
            "unique_id": f"{self.device_id}_{key}",
            "object_id": f"{self.device_id}_{key}",
            "device": self._device(),
            "state_topic": f"{base}/state",
            "availability_topic": f"{self.topic_prefix}/availability",
        }
        if unit:
            common["unit_of_measurement"] = unit
        if self.ha_discovery:
            topic = f"{self.ha_prefix}/sensor/{self.device_id}/{key}/config"
            self.client.publish(topic, json.dumps(common), retain=True)

    def publish_param_discovery(self, index: int, name: str, title: str, unit: str,
                              writable: bool, mappings: dict[int, str] | None = None,
                              is_bool: bool = False) -> None:
        base = f"{self.topic_prefix}/param_{index:02x}"
        common = {
            "name": title,
            "unique_id": f"{self.device_id}_param_{index:02x}",
            "object_id": f"{self.device_id}_param_{index:02x}",
            "device": self._device(),
            "state_topic": f"{base}/state",
            "availability_topic": f"{self.topic_prefix}/availability",
        }
        if unit:
            common["unit_of_measurement"] = unit
        if self.ha_discovery:
            if writable and mappings:
                cfg = {**common, "command_topic": f"{base}/set",
                       "options": sorted(mappings.values())}
                component = "select"
            elif writable and is_bool:
                cfg = {**common, "command_topic": f"{base}/set",
                       "payload_on": "on", "payload_off": "off"}
                component = "switch"
            elif writable:
                cfg = {**common, "command_topic": f"{base}/set"}
                component = "number"
            else:
                dev_class, state_class = _UNIT_DEVICE_CLASS.get(unit, (None, "measurement"))
                cfg = {**common}
                if dev_class:
                    cfg["device_class"] = dev_class
                if state_class:
                    cfg["state_class"] = state_class
                component = "sensor"
            topic = f"{self.ha_prefix}/{component}/{self.device_id}/param_{index:02x}/config"
            self.client.publish(topic, json.dumps(cfg), retain=True)
        self._param_topics[f"{base}/set"] = index

    def publish_param_state(self, index: int, value) -> None:
        payload = value if isinstance(value, str) else json.dumps(value)
        self.client.publish(f"{self.topic_prefix}/param_{index:02x}/state", payload,
                            retain=True)

    # -- callbacks ------------------------------------------------------------

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        log.info("MQTT on_connect: %s", reason_code)
        client.subscribe(f"{self.topic_prefix}/+/set")
        client.publish(f"{self.topic_prefix}/availability", "online", retain=True)

    def publish_knob_discovery(self, center: int = 0x85) -> None:
        """Slider for the display's temperature knob (raw position 0-255)."""
        base = f"{self.topic_prefix}/knob"
        cfg = {
            "name": "Värmekurva förskjutning (knob)",
            "unique_id": f"{self.device_id}_knob",
            "object_id": f"{self.device_id}_knob",
            "device": self._device(),
            "state_topic": f"{base}/state",
            "command_topic": f"{base}/set",
            "availability_topic": f"{self.topic_prefix}/availability",
            "min": 0, "max": 255, "step": 1,
            "mode": "slider",
            "icon": "mdi:rotate-right",
        }
        if self.ha_discovery:
            topic = f"{self.ha_prefix}/number/{self.device_id}/knob/config"
            self.client.publish(topic, json.dumps(cfg), retain=True)
        self.client.publish(f"{base}/state", str(center), retain=True)

    def publish_knob_state(self, pos: int) -> None:
        self.client.publish(f"{self.topic_prefix}/knob/state", str(pos), retain=True)

    def _on_message(self, client, userdata, msg) -> None:
        if msg.topic == f"{self.topic_prefix}/knob/set" and self.on_knob is not None:
            try:
                self.on_knob(int(float(msg.payload.decode())))
            except Exception:
                log.exception("knob command failed")
            return
        if msg.topic in self._param_topics and self.on_param_command is not None:
            payload = msg.payload.decode("utf-8", "replace")
            log.info("param command %#04x = %s", self._param_topics[msg.topic], payload)
            try:
                self.on_param_command(self._param_topics[msg.topic], payload)
            except Exception:
                log.exception("param command failed for %#x", self._param_topics[msg.topic])
            return
        address = self._by_topic.get(msg.topic)
        if address is None or self.on_command is None:
            return
        payload = msg.payload.decode("utf-8", "replace")
        log.info("command for register %d (%s): %s", address, msg.topic, payload)
        try:
            self.on_command(address, payload)
        except Exception:
            log.exception("command handling failed for %d", address)
