"""Runtime configuration (env vars with NIBEMOTE_ prefix, or a .env file)."""

from __future__ import annotations

from pydantic_settings import BaseSettings, SettingsConfigDict


def _parse_addresses(value: str | list[int]) -> list[int]:
    if isinstance(value, str):
        out = []
        for part in value.split(","):
            part = part.strip()
            if part:
                out.append(int(part, 0))
        return out
    return list(value)


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="NIBEMOTE_", env_file=".env", extra="ignore")

    # serial / bus
    serial_port: str = "/dev/ttyUSB0"
    baud: int = 19200
    parity: str = "N"
    stopbits: float = 1
    # comma separated list of bus addresses we answer as (kept a plain string so
    # pydantic-settings doesn't try to JSON-decode the env var)
    ack_addresses: str = "0xF9,0x14"
    # true = pure listener: never transmit on the bus (use when the real display
    # is still attached, to avoid ACK collisions / "loss of communication")
    passive: bool = False
    rs485_rts: bool = True

    # mqtt / home assistant
    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_user: str = ""
    mqtt_password: str = ""
    mqtt_topic_prefix: str = "nibemote"
    ha_discovery_prefix: str = "homeassistant"
    ha_discovery: bool = True
    device_name: str = "Nibe VVM300"
    device_id: str = "nibemote_vvm300"

    # behaviour
    poll_interval: float = 5.0
    read_timeout: float = 10.0
    word_swap: bool = False
    registers_file: str | None = None
    log_level: str = "INFO"

    @property
    def addresses(self) -> list[int]:
        return _parse_addresses(self.ack_addresses)


_settings: Settings | None = None


def get_settings() -> Settings:
    global _settings
    if _settings is None:
        _settings = Settings()
    return _settings
