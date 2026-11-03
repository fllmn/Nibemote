"""RS485 serial transport (19200 8N1 half duplex)."""

from __future__ import annotations

import logging
import time

import serial
import serial.rs485  # noqa: F401  (registers the rs485_mode attribute)

log = logging.getLogger(__name__)


class SerialBus:
    """Thin wrapper around pyserial with RS485 direction handling.

    On Linux, pyserial exposes ``rs485_mode`` which drives RTS for the
    transmit direction — this is what most USB/RS485 adapters and RPi HATs
    expect. Set ``rs485_rts=False`` for adapters with automatic direction
    control.
    """

    def __init__(
        self,
        port: str,
        baud: int = 19200,
        bytesize: int = 8,
        parity: str = "N",
        stopbits: float = 1,
        rs485_rts: bool = True,
        read_timeout: float = 0.05,
    ) -> None:
        self.port = port
        self.baud = baud
        self.bytesize = bytesize
        self.parity = parity
        self.stopbits = stopbits
        self.rs485_rts = rs485_rts
        self.read_timeout = read_timeout
        self._ser: serial.Serial | None = None

    def open(self) -> None:
        self._ser = serial.Serial(
            port=self.port,
            baudrate=self.baud,
            bytesize=self.bytesize,
            parity=self.parity,
            stopbits=self.stopbits,
            timeout=self.read_timeout,
            write_timeout=1.0,
        )
        if self.rs485_rts:
            try:
                # Drive RTS high while transmitting, low while receiving.
                self._ser.rs485_mode = serial.rs485.RS485Settings(
                    rts_level_for_tx=1, rts_level_for_rx=0
                )
                log.info("RS485 RTS direction control enabled on %s", self.port)
            except (AttributeError, OSError, ValueError) as exc:
                log.warning("Could not enable rs485_mode on %s (%s); "
                            "assuming automatic direction control", self.port, exc)
        log.info(
            "Opened %s at %d %d%s%s",
            self.port, self.baud, self.bytesize, self.parity, self.stopbits,
        )

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    @property
    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def read(self, max_bytes: int = 4096) -> bytes:
        """Read whatever is available; blocks up to read_timeout for one byte.

        Uses in_waiting so a poll byte is returned immediately (the Nibe master
        expects the ACK within a few ms — a plain read(max_bytes) would always
        wait out the full timeout).
        """
        if self._ser is None:
            raise RuntimeError("bus not open")
        waiting = self._ser.in_waiting
        if waiting:
            return self._ser.read(min(waiting, max_bytes))
        return self._ser.read(1)

    def write(self, data: bytes) -> None:
        """Send bytes and wait until they have left the UART (half duplex)."""
        if self._ser is None:
            raise RuntimeError("bus not open")
        self._ser.write(data)
        self._ser.flush()
        # give the last byte time to physically shift out before releasing RTS
        time.sleep(len(data) * 10 / self.baud + 0.002)

    def reset_buffers(self) -> None:
        if self._ser is not None:
            self._ser.reset_input_buffer()
            self._ser.reset_output_buffer()

    # context manager sugar
    def __enter__(self) -> "SerialBus":
        self.open()
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()
