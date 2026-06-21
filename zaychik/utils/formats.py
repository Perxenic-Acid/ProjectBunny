"""DXGI format descriptors and typed decoders for vertex/index buffer contents.

Each FormatInfo tells us the byte size per element and provides a decoder that
turns ``bytes`` at a given offset into a tuple of float/int components. We only
populate the formats that modern D3D12 games (and UE5 in particular) emit for
vertex and index streams; compressed texture formats are out of scope.

Reference:
- https://learn.microsoft.com/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format
- SSMT's hardcoded format tables (ssmt4/src-tauri/src/constants/gametype_format.rs).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Callable, Dict, Optional, Tuple


# A decoded element is a tuple of floats (for unorm/snorm/float types) or ints
# (for uint/sint types).
Scalar = float
Components = Tuple


@dataclass(frozen=True)
class FormatInfo:
    name: str
    byte_width: int
    component_count: int
    is_integer: bool
    decode: Callable[[bytes, int], Components]


# ---- Component decoders -----------------------------------------------------


def _u8_to_unorm(b: int) -> float:
    return b / 255.0


def _s8_to_snorm(b: int) -> float:
    return max(-1.0, b / 127.0)


def _u16_to_unorm(v: int) -> float:
    return v / 65535.0


def _s16_to_snorm(v: int) -> float:
    return max(-1.0, v / 32767.0)


def _mk_unpack(st: str, count: int, is_integer: bool) -> Tuple[int, int, bool, Callable]:
    fmt = "<" + st
    bw = struct.calcsize(fmt)

    def decode(data: bytes, offset: int) -> Components:
        return struct.unpack_from(fmt, data, offset)

    return bw, count, is_integer, decode


# ---- Build the table --------------------------------------------------------


def _build() -> Dict[str, FormatInfo]:
    table: Dict[str, FormatInfo] = {}

    def add(name: str, byte_width: int, component_count: int, is_integer: bool,
            decode: Callable[[bytes, int], Components]):
        table[name] = FormatInfo(name, byte_width, component_count, is_integer, decode)

    add("DXGI_FORMAT_UNKNOWN", 0, 0, False, lambda d, o: ())

    # Indices and scalar ints
    add("DXGI_FORMAT_R32_UINT", *_mk_unpack("I", 1, True))
    add("DXGI_FORMAT_R16_UINT", *_mk_unpack("H", 1, True))
    add("DXGI_FORMAT_R8_UINT", 1, 1, True, lambda d, o: (d[o],))
    add("DXGI_FORMAT_R8_SINT", 1, 1, True, lambda d, o: struct.unpack_from("<b", d, o))

    # Float 32
    add("DXGI_FORMAT_R32_FLOAT", *_mk_unpack("f", 1, False))
    add("DXGI_FORMAT_R32G32_FLOAT", *_mk_unpack("2f", 2, False))
    add("DXGI_FORMAT_R32G32B32_FLOAT", *_mk_unpack("3f", 3, False))
    add("DXGI_FORMAT_R32G32B32A32_FLOAT", *_mk_unpack("4f", 4, False))

    # SInt/UInt 32
    add("DXGI_FORMAT_R32_SINT", *_mk_unpack("i", 1, True))
    add("DXGI_FORMAT_R32G32_SINT", *_mk_unpack("2i", 2, True))
    add("DXGI_FORMAT_R32G32B32_SINT", *_mk_unpack("3i", 3, True))
    add("DXGI_FORMAT_R32G32B32A32_SINT", *_mk_unpack("4i", 4, True))
    add("DXGI_FORMAT_R32G32_UINT", *_mk_unpack("2I", 2, True))
    add("DXGI_FORMAT_R32G32B32_UINT", *_mk_unpack("3I", 3, True))
    add("DXGI_FORMAT_R32G32B32A32_UINT", *_mk_unpack("4I", 4, True))

    # Float16 (half)
    add("DXGI_FORMAT_R16_FLOAT", 2, 1, False, lambda d, o: struct.unpack_from("<e", d, o))
    add("DXGI_FORMAT_R16G16_FLOAT", 4, 2, False, lambda d, o: struct.unpack_from("<ee", d, o))
    add("DXGI_FORMAT_R16G16B16A16_FLOAT", 8, 4, False, lambda d, o: struct.unpack_from("<eeee", d, o))

    # 16-bit UNORM/SNORM/UINT/SINT
    add("DXGI_FORMAT_R16_UNORM", 2, 1, False,
        lambda d, o: (_u16_to_unorm(struct.unpack_from("<H", d, o)[0]),))
    add("DXGI_FORMAT_R16_SNORM", 2, 1, False,
        lambda d, o: (_s16_to_snorm(struct.unpack_from("<h", d, o)[0]),))
    add("DXGI_FORMAT_R16G16_UNORM", 4, 2, False,
        lambda d, o: (_u16_to_unorm(struct.unpack_from("<H", d, o)[0]),
                      _u16_to_unorm(struct.unpack_from("<H", d, o + 2)[0])))
    add("DXGI_FORMAT_R16G16_SNORM", 4, 2, False,
        lambda d, o: (_s16_to_snorm(struct.unpack_from("<h", d, o)[0]),
                      _s16_to_snorm(struct.unpack_from("<h", d, o + 2)[0])))
    add("DXGI_FORMAT_R16G16B16A16_UNORM", 8, 4, False,
        lambda d, o: tuple(_u16_to_unorm(struct.unpack_from("<H", d, o + 2 * i)[0])
                           for i in range(4)))
    add("DXGI_FORMAT_R16G16B16A16_SNORM", 8, 4, False,
        lambda d, o: tuple(_s16_to_snorm(struct.unpack_from("<h", d, o + 2 * i)[0])
                           for i in range(4)))
    add("DXGI_FORMAT_R16_SINT", *_mk_unpack("h", 1, True))
    add("DXGI_FORMAT_R16G16_SINT", *_mk_unpack("2h", 2, True))
    add("DXGI_FORMAT_R16G16B16A16_SINT", *_mk_unpack("4h", 4, True))
    add("DXGI_FORMAT_R16G16_UINT", *_mk_unpack("2H", 2, True))
    add("DXGI_FORMAT_R16G16B16A16_UINT", *_mk_unpack("4H", 4, True))

    # 8-bit per channel
    add("DXGI_FORMAT_R8_UNORM", 1, 1, False, lambda d, o: (_u8_to_unorm(d[o]),))
    add("DXGI_FORMAT_R8_SNORM", 1, 1, False,
        lambda d, o: (_s8_to_snorm(struct.unpack_from("<b", d, o)[0]),))
    add("DXGI_FORMAT_R8G8_UNORM", 2, 2, False,
        lambda d, o: (_u8_to_unorm(d[o]), _u8_to_unorm(d[o + 1])))
    add("DXGI_FORMAT_R8G8_SNORM", 2, 2, False,
        lambda d, o: (_s8_to_snorm(struct.unpack_from("<b", d, o)[0]),
                      _s8_to_snorm(struct.unpack_from("<b", d, o + 2)[0])))

    def _decode_rgba8_unorm(d, o):
        return (_u8_to_unorm(d[o]), _u8_to_unorm(d[o + 1]),
                _u8_to_unorm(d[o + 2]), _u8_to_unorm(d[o + 3]))

    def _decode_rgba8_snorm(d, o):
        b0, b1, b2, b3 = struct.unpack_from("<4b", d, o)
        return (_s8_to_snorm(b0), _s8_to_snorm(b1),
                _s8_to_snorm(b2), _s8_to_snorm(b3))

    add("DXGI_FORMAT_R8G8B8A8_UNORM", 4, 4, False, _decode_rgba8_unorm)
    add("DXGI_FORMAT_R8G8B8A8_SNORM", 4, 4, False, _decode_rgba8_snorm)
    add("DXGI_FORMAT_R8G8B8A8_UINT", 4, 4, True,
        lambda d, o: (d[o], d[o + 1], d[o + 2], d[o + 3]))
    add("DXGI_FORMAT_R8G8B8A8_SINT", 4, 4, True,
        lambda d, o: struct.unpack_from("<4b", d, o))
    add("DXGI_FORMAT_B8G8R8A8_UNORM", 4, 4, False,
        lambda d, o: (_u8_to_unorm(d[o + 2]), _u8_to_unorm(d[o + 1]),
                      _u8_to_unorm(d[o]), _u8_to_unorm(d[o + 3])))

    return table


FORMATS: Dict[str, FormatInfo] = _build()


def get_format(name: Optional[str]) -> Optional[FormatInfo]:
    """Look up a DXGI format by name; returns None for UNKNOWN / missing."""
    if not name:
        return None
    return FORMATS.get(name.upper())
