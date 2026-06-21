"""Vertex layout presets and matching logic.

A :class:`VertexElement` declares one vertex attribute: semantic name, DXGI
format, byte offset within the stream, and slot index.  A :class:`StreamLayout`
declares the full set of elements that live in one vertex buffer slot along
with the expected total stride.  A :class:`VertexFactory` is a complete input
layout (a set of stream layouts covering all slots used by a draw) plus
metadata and a :meth:`~VertexFactory.match` method.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Optional, Tuple

from .formats import DxgiFormat, FormatInfo


# Standard D3D semantic identifiers
SEMANTIC_POSITION = "POSITION"
SEMANTIC_NORMAL = "NORMAL"
SEMANTIC_TANGENT = "TANGENT"
SEMANTIC_TEXCOORD = "TEXCOORD"
SEMANTIC_COLOR = "COLOR"
SEMANTIC_BLENDINDICES = "BLENDINDICES"
SEMANTIC_BLENDWEIGHTS = "BLENDWEIGHTS"


@dataclass(frozen=True)
class VertexElement:
    """One element/attribute within a vertex stream."""

    semantic: str
    semantic_index: int
    format_name: str
    offset: int
    slot: int
    format_info: FormatInfo = field(init=False)

    def __post_init__(self) -> None:
        info = DxgiFormat.get(self.format_name)
        if info is None:
            raise ValueError(f"Unknown DXGI format {self.format_name}")
        object.__setattr__(self, "format_info", info)

    @property
    def byte_width(self) -> int:
        return self.format_info.byte_width


@dataclass(frozen=True)
class StreamLayout:
    """Expected layout of one vertex buffer slot."""

    slot: int
    stride: int
    elements: Tuple[VertexElement, ...]
    expected_format: Optional[str] = None

    def matches(self, actual_stride: int, actual_format: Optional[str]) -> bool:
        if self.stride != actual_stride:
            return False
        if self.expected_format is None:
            return True
        if not actual_format or actual_format == "DXGI_FORMAT_UNKNOWN":
            return True
        return self.expected_format.upper() == actual_format.upper()


@dataclass(frozen=True)
class VertexFactory:
    """A complete input layout plus matching logic and the built-in UE5 presets.

    All builder helpers (``make_element`` / ``make_stream``) are classmethods so
    presets at module bottom construct themselves without free functions.
    ``match()`` is also a classmethod that selects the best-scoring preset for
    a given (slots, ib_format) pair.
    """

    name: str
    engine: str
    category: str
    game: str
    streams: Tuple[StreamLayout, ...]
    index_format: Optional[str] = None
    notes: str = ""

    # ----- Matching -----

    def required_slots(self) -> Dict[int, StreamLayout]:
        return {stream.slot: stream for stream in self.streams}

    def score_for_draw(self,
                       slot_info: Dict[int, Tuple[int, Optional[str]]],
                       ib_format: Optional[str]) -> int:
        required = self.required_slots()
        for slot, stream in required.items():
            info = slot_info.get(slot)
            if info is None:
                return 0
            actual_stride, actual_fmt = info
            if not stream.matches(actual_stride, actual_fmt):
                return 0
        if self.index_format is not None and ib_format is not None and self.index_format != ib_format:
            return 0
        return len(required)

    # ----- Preset registry -----

    _REGISTRY: Tuple["VertexFactory", ...] = ()

    @classmethod
    def register(cls, *factories: "VertexFactory") -> None:
        cls._REGISTRY = factories

    @classmethod
    def registry(cls) -> Tuple["VertexFactory", ...]:
        return cls._REGISTRY

    @classmethod
    def match(cls,
              slot_info: Dict[int, Tuple[int, Optional[str]]],
              ib_format: Optional[str],
              preset: str = "auto") -> Optional["VertexFactory"]:
        """Pick the highest-scoring factory from the registry."""
        best_score = 0
        best_factory: Optional[VertexFactory] = None
        for factory in cls._REGISTRY:
            if not factory.matches_preset(preset):
                continue
            score = factory.score_for_draw(slot_info, ib_format)
            if score > best_score:
                best_score = score
                best_factory = factory
        return best_factory

    def matches_preset(self, preset: str) -> bool:
        if preset in ("", "auto"):
            return True
        if preset == "generic":
            return self.game == "generic"
        return self.game == preset

    # ----- Builder helpers for declaring presets -----

    @staticmethod
    def element(semantic: str, semantic_index: int, fmt: str,
                slot: int, offset: int) -> VertexElement:
        return VertexElement(semantic, semantic_index, fmt, offset, slot)

    @staticmethod
    def stream(slot: int, stride: int, *elements: VertexElement,
               expected_format: Optional[str] = None) -> StreamLayout:
        used = 0
        for e in elements:
            assert e.slot == slot, f"element slot {e.slot} != stream slot {slot}"
            assert e.offset + e.byte_width <= stride, (
                f"element {e.semantic}{e.semantic_index} @{e.offset} "
                f"size {e.byte_width} overflows stream {slot} stride {stride}")
            used = max(used, e.offset + e.byte_width)
        assert used <= stride
        return StreamLayout(slot, stride, elements, expected_format=expected_format)


# ---------------------------------------------------------------------------
# UE5 factory presets
# ---------------------------------------------------------------------------
E = VertexFactory.element
S = VertexFactory.stream

UE5_STATIC_MINIMAL = VertexFactory(
    name="UE5.StaticMinimal",
    engine="UE5",
    category="static",
    game="ue5",
    notes="Position-only static draws (UI quads, post, shadows) with packed TBN in 4-byte stream.",
    streams=(
        S(0, 12, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        S(1, 4,  E(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0)),
    ),
)

UE5_STATIC_FULL = VertexFactory(
    name="UE5.StaticFull",
    engine="UE5",
    category="static",
    game="ue5",
    notes="UE5 static mesh: pos3f + packed TBN snorm8x8 + 4 half2 UVs.",
    streams=(
        S(0, 12, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        S(1, 8,
          E(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0),
          E(SEMANTIC_NORMAL, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 4)),
        S(4, 16,
          E(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0),
          E(SEMANTIC_TEXCOORD, 1, "DXGI_FORMAT_R16G16_FLOAT", 4, 4),
          E(SEMANTIC_TEXCOORD, 2, "DXGI_FORMAT_R16G16_FLOAT", 4, 8),
          E(SEMANTIC_TEXCOORD, 3, "DXGI_FORMAT_R16G16_FLOAT", 4, 12)),
    ),
)

UE5_STATIC_UV8 = VertexFactory(
    name="UE5.StaticUV8",
    engine="UE5",
    category="static",
    game="ue5",
    notes="UE5 static mesh with single half2 UV stream (stride 8).",
    streams=(
        S(0, 12, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        S(1, 8,
          E(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0),
          E(SEMANTIC_NORMAL, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 4)),
        S(4, 8, E(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0)),
    ),
)

UE5_STATIC_TANGENT_ONLY = VertexFactory(
    name="UE5.StaticTangentOnly",
    engine="UE5",
    category="static",
    game="ue5",
    notes="UE5 static path with 4-byte TANGENT stream, no NORMAL stream (depth/shadow).",
    streams=(
        S(0, 12, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        S(1, 4,  E(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0)),
        S(4, 8,  E(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0)),
    ),
)

UE5_GPU_SKINNED_FULL = VertexFactory(
    name="UE5.GPUSkinnedFull",
    engine="UE5",
    category="gpu_preskinning",
    game="ue5",
    notes="GPU pre-skinned mesh: pos+TBN+4 UVs (post-skin pose, same streams as static).",
    streams=UE5_STATIC_FULL.streams,
)

UE5_GPU_SKINNED_EXPANDED = VertexFactory(
    name="UE5.GPUSkinnedExpanded",
    engine="UE5",
    category="gpu_preskinning",
    game="ue5",
    notes="GPU pre-skinned: slot0 stride=32 (first 12b = post-skin position).",
    streams=(
        S(0, 32, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        S(1, 8,
          E(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0),
          E(SEMANTIC_NORMAL, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 4)),
        S(4, 16,
          E(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0),
          E(SEMANTIC_TEXCOORD, 1, "DXGI_FORMAT_R16G16_FLOAT", 4, 4),
          E(SEMANTIC_TEXCOORD, 2, "DXGI_FORMAT_R16G16_FLOAT", 4, 8),
          E(SEMANTIC_TEXCOORD, 3, "DXGI_FORMAT_R16G16_FLOAT", 4, 12)),
    ),
)

GENERIC_POSITION_ONLY_S12 = VertexFactory(
    name="Generic.PositionOnly",
    engine="generic",
    category="fallback",
    game="generic",
    notes="Fallback: slot0 stride 12 POSITION float3.",
    streams=(S(0, 12, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),),
)

GENERIC_POSITION_ONLY_S16 = VertexFactory(
    name="Generic.PositionOnlyS16",
    engine="generic",
    category="fallback",
    game="generic",
    notes="Fallback: slot0 stride 16, POSITION float3 at offset 0.",
    streams=(S(0, 16, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),),
)

GENERIC_POSITION_ONLY_S32 = VertexFactory(
    name="Generic.PositionOnlyS32",
    engine="generic",
    category="fallback",
    game="generic",
    notes="Fallback: slot0 stride 32, POSITION float3 at offset 0.",
    streams=(S(0, 32, E(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),),
)

STELLAR_BLADE_STATIC_FULL = VertexFactory(
    name="StellarBlade.StaticFull",
    engine="UE5",
    category="static",
    game="stellar_blade",
    notes="Stellar Blade main mesh preset: vb0 POSITION float3, vb1 packed tangent/normal, vb4 half UV sets.",
    streams=UE5_STATIC_FULL.streams,
)

STELLAR_BLADE_STATIC_UV8 = VertexFactory(
    name="StellarBlade.StaticUV8",
    engine="UE5",
    category="static",
    game="stellar_blade",
    notes="Stellar Blade static mesh with compact UV stream.",
    streams=UE5_STATIC_UV8.streams,
)

STELLAR_BLADE_TANGENT_ONLY = VertexFactory(
    name="StellarBlade.TangentOnly",
    engine="UE5",
    category="static",
    game="stellar_blade",
    notes="Stellar Blade depth/shadow style mesh: POSITION plus tangent stream.",
    streams=UE5_STATIC_TANGENT_ONLY.streams,
)

STELLAR_BLADE_MINIMAL = VertexFactory(
    name="StellarBlade.Minimal",
    engine="UE5",
    category="static",
    game="stellar_blade",
    notes="Stellar Blade minimal mesh: vb0 POSITION float3, vb1 packed tangent.",
    streams=UE5_STATIC_MINIMAL.streams,
)

STELLAR_BLADE_GPU_SKINNED_EXPANDED = VertexFactory(
    name="StellarBlade.GPUSkinnedExpanded",
    engine="UE5",
    category="gpu_preskinning",
    game="stellar_blade",
    notes="Stellar Blade GPU pre-skinned mesh: slot0 stride 32, position at offset 0.",
    streams=UE5_GPU_SKINNED_EXPANDED.streams,
)

# Order matters: more specific factories first so same-score ties pick the
# richer definition.
VertexFactory.register(
    STELLAR_BLADE_STATIC_FULL,
    STELLAR_BLADE_STATIC_UV8,
    STELLAR_BLADE_TANGENT_ONLY,
    STELLAR_BLADE_MINIMAL,
    STELLAR_BLADE_GPU_SKINNED_EXPANDED,
    UE5_STATIC_FULL,
    UE5_STATIC_UV8,
    UE5_STATIC_TANGENT_ONLY,
    UE5_STATIC_MINIMAL,
    UE5_GPU_SKINNED_FULL,
    UE5_GPU_SKINNED_EXPANDED,
    GENERIC_POSITION_ONLY_S12,
    GENERIC_POSITION_ONLY_S16,
    GENERIC_POSITION_ONLY_S32,
)
