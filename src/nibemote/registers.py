"""Register (coil) map for Nibe VVM-series indoor units.

The bundled ``data/vvm_registers.json`` is the VVM225/VVM320/VVM325 map from the
open-source `nibe` library (yozik04/nibe) — 884 registers, 466 writable. It is
the closest published map for a VVM300 + F2025; registers that do not exist on
the F2025 simply never answer and are skipped at runtime.

Addresses are the "coil" addresses used on the Nibe bus (40004..49xxx etc.).
Values on the wire are little-endian; 32-bit values may need word swap
(service menu 5.3.11 on the pump) — controlled by ``word_swap``.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from importlib import resources
from pathlib import Path

_STRUCT_FMT = {
    "u8": ("<B", 1),
    "s8": ("<b", 1),
    "u16": ("<H", 2),
    "s16": ("<h", 2),
    "u32": ("<I", 4),
    "s32": ("<i", 4),
}


@dataclass
class Register:
    address: int
    name: str
    title: str
    info: str = ""
    unit: str = ""
    size: str = "s16"
    factor: float = 1.0
    min: float | None = None
    max: float | None = None
    default: float | None = None
    writable: bool = False
    mappings: dict[str, str] = field(default_factory=dict)

    @property
    def nbytes(self) -> int:
        return _STRUCT_FMT[self.size][1]

    @property
    def is_boolean(self) -> bool:
        return self.factor == 1 and self.min == 0 and self.max == 1 and not self.mappings

    @property
    def is_enum(self) -> bool:
        return bool(self.mappings)

    def _swap_words(self, raw: bytes) -> bytes:
        if len(raw) == 4:
            return raw[2:4] + raw[0:2]
        return raw

    def decode(self, raw: bytes, word_swap: bool = False) -> float | int | str:
        """Decode raw wire bytes to a scaled value (or enum label)."""
        if word_swap:
            raw = self._swap_words(raw)
        fmt, _ = _STRUCT_FMT[self.size]
        raw = raw[: struct.calcsize(fmt)].ljust(struct.calcsize(fmt), b"\x00")
        (value,) = struct.unpack(fmt, raw)
        if self.mappings:
            return self.mappings.get(str(value), str(value))
        if self.factor and self.factor != 1:
            return round(value / self.factor, 3)
        return value

    def encode(self, value: float | int | str, word_swap: bool = False) -> bytes:
        """Encode a scaled value (or enum label) to raw wire bytes."""
        if self.mappings:
            reverse = {v: k for k, v in self.mappings.items()}
            key = reverse.get(str(value))
            if key is None:
                raise ValueError(f"{value!r} is not a valid mapping for {self.name}")
            raw_value = int(key)
        else:
            raw_value = int(round(float(value) * self.factor))
        fmt, size = _STRUCT_FMT[self.size]
        raw = struct.pack(fmt, raw_value & ((1 << (8 * size)) - 1) if fmt.isupper() else raw_value)
        if word_swap:
            raw = self._swap_words(raw)
        return raw


def _register_from(address: int, d: dict) -> Register:
    return Register(
        address=address,
        name=d.get("name") or f"register-{address}",
        title=d.get("title") or f"Register {address}",
        info=d.get("info", ""),
        unit=d.get("unit", ""),
        size=d.get("size", "s16"),
        factor=float(d.get("factor", 1)),
        min=d.get("min"),
        max=d.get("max"),
        default=d.get("default"),
        writable=bool(d.get("write", False)),
        mappings={str(k): v for k, v in d.get("mappings", {}).items()},
    )


def _slug(title: str) -> str:
    import re
    s = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
    return s or "reg"


def _load_tsv(path) -> dict[int, Register]:
    import csv
    out: dict[int, Register] = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh, delimiter="\t"):
            if len(row) < 9:
                continue
            addr, title, unit, size, factor, mn, mx, write, mappings = row[:9]
            addr_i = int(addr)
            out[addr_i] = Register(
                address=addr_i,
                name=f"{_slug(title)}-{addr_i}",
                title=title,
                unit=unit,
                size=size or "s16",
                factor=float(factor or 1),
                min=float(mn) if mn else None,
                max=float(mx) if mx else None,
                writable=write == "true",
                mappings={str(k): v for k, v in json.loads(mappings or "{}").items()},
            )
    return out


def load_registers(path: str | Path | None = None) -> dict[int, Register]:
    """Load the register map; defaults to the bundled VVM map (TSV, JSON fallback)."""
    if path is None:
        base = resources.files("nibemote").joinpath("data")
        tsv = base.joinpath("vvm_registers.tsv")
        if tsv.is_file():
            return _load_tsv(str(tsv))
        with base.joinpath("vvm_registers.json").open("r", encoding="utf-8") as fh:
            raw = json.load(fh)
    else:
        p = Path(path)
        if p.suffix == ".tsv":
            return _load_tsv(p)
        with open(p, "r", encoding="utf-8") as fh:
            raw = json.load(fh)
    return {int(addr): _register_from(int(addr), d) for addr, d in raw.items()}
