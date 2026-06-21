"""Vertex layout presets and matching logic.

This is the "strict, not guessing" replacement for the old stride-only
heuristic.  A :class:`VertexElement` declares one vertex attribute: its
semantic name, DXGI format, byte offset within the stream, and which vertex
buffer slot it lives in.  A :class:`StreamLayout` declares the full set of
elements that live in one vertex buffer slot, along with the expected total
stride.  A :class:`VertexFactory` is a complete vertex input layout (a set of
stream layouts covering all slots used by a draw) plus friendly metadata.

Matching is performed per-draw: a factory fits the draw only when every slot
it declares is present with exactly the expected stride.  Slots the draw has
but the factory does not declare are tolerated as "extra" streams (skinning
constants, instance data, per-pass extras) so we do not reject a draw just
because a tooling/pass-specific stream is attached.

These factories reflect what we actually observe in UE5 Stellar Blade DX12
captures.  As we encounter more engines/games we can grow the table; the
matcher always picks the factory whose required slots cover the most of the
draw's vertex streams (so richer, more-specific factories win over coarse
fallbacks).

Naming convention matches D3D11 semantic names (POSITION, NORMAL, TANGENT,
TEXCOORD, COLOR, BLENDINDICES, BLENDWEIGHTS) so the importer can generically
route them without hardcoding factory names.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from .formats import FormatInfo, get_format


# Standard semantic identifiers
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
    semantic_index: int  # TEXCOORD0, TEXCOORD1, ...
    format_name: str     # DXGI format name (e.g. "DXGI_FORMAT_R32G32B32_FLOAT")
    offset: int          # byte offset within each vertex
    slot: int            # vertex buffer slot
    format_info: FormatInfo = field(init=False)

    def __post_init__(self) -> None:
        info = get_format(self.format_name)
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

    def matches_stride(self, actual_stride: int) -> bool:
        return self.stride == actual_stride


@dataclass(frozen=True)
class VertexFactory:
    """A complete input layout: which slots hold which elements."""

    name: str
    engine: str                   # e.g. "UE5", "generic"
    category: str                 # "static", "skinned", "debug", ...
    streams: Tuple[StreamLayout, ...]
    # Index buffer format we expect, or None = accept any.
    index_format: Optional[str] = None
    # Optional notes for debugging/UI
    notes: str = ""

    def required_slots(self) -> Dict[int, StreamLayout]:
        return {stream.slot: stream for stream in self.streams}

    def score_for_draw(self, slot_strides: Dict[int, int],
                       ib_format: Optional[str]) -> int:
        """Return a match score >= 0; higher is better, 0 = does not fit.

        Scores by the count of slots that match exactly. Factories that
        declare more required slots rank higher if they all fit.
        """
        required = self.required_slots()
        for slot, stream in required.items():
            actual = slot_strides.get(slot)
            if actual is None:
                return 0
            if not stream.matches_stride(actual):
                return 0
        if self.index_format is not None and ib_format is not None and self.index_format != ib_format:
            return 0
        return len(required)


# ---- UE5 factory presets ----------------------------------------------------
#
# UE5 Static Mesh / FStaticMeshVertexBuffer / FLocalVertexFactory observed
# layouts.  Stellar Blade DX12 captures use per-component streams (Position in
# one slot, packed TBN in another, UVs in another, color in another), which is
# the UE "multiple streams" path.  Concrete slot assignments (0=pos, 1=TBN,
# 2/3=misc, 4=UVs) are confirmed empirically from the dump.


def _e(semantic: str, si: int, fmt: str, slot: int, offset: int) -> VertexElement:
    return VertexElement(semantic, si, fmt, offset, slot)


def _stream(slot: int, stride: int, *elements: VertexElement) -> StreamLayout:
    # Sanity check: element offsets + sizes fit within stride.
    used = 0
    for e in elements:
        assert e.slot == slot, f"element slot {e.slot} != stream slot {slot}"
        assert e.offset + e.byte_width <= stride, (
            f"element {e.semantic}{e.semantic_index} @{e.offset} size {e.byte_width} "
            f"overflows stream {slot} stride {stride}")
        used = max(used, e.offset + e.byte_width)
    assert used <= stride
    return StreamLayout(slot, stride, elements)


#: UE5 static mesh, static (non-pose) path, position-only + TBN packed + color.
#: Observed on clear/quad draws and many small static draws
#:   slot0 stride=12 float3 POSITION
#:   slot1 stride=4  R8G8B8A8_SNORM TANGENT (with normal coming from elsewhere or flat-shaded)
#:   slot2 stride=4  R8G8B8A8_UNORM COLOR  (white/constant on many draws)
#:   slot3 stride=4  R8G8B8A8_UNORM COLOR2 / mask (often 0xFFFFFFFF)
UE5_STATIC_MINIMAL = VertexFactory(
    name="UE5.StaticMinimal",
    engine="UE5",
    category="static",
    notes="Position-only static draws (UI quads, post, shadows) with packed TBN in single 4-byte stream.",
    streams=(
        _stream(0, 12, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        _stream(1, 4, _e(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0)),
    ),
)

#: UE5 static mesh with TANGENT+NORMAL packed as two R8G8B8A8_SNORM in one 8-byte
#: stream (the standard UE5 FStaticMeshVertexBuffer packed-TangentX-Normal layout).
#:   slot0 stride=12  POSITION float3
#:   slot1 stride=8   TANGENT snorm8x3 (off 0) + NORMAL snorm8x3 (off 4)
#:   slot2 stride=4   COLOR/aux (not decoded semantically)
#:   slot3 stride=4   aux/skinning mask
#:   slot4 stride=16  TEXCOORD0..3 as half2 pairs (4 UVs @ 4 bytes each = 16 bytes)
UE5_STATIC_FULL = VertexFactory(
    name="UE5.StaticFull",
    engine="UE5",
    category="static",
    notes="UE5 static mesh: pos0 + packed TBN8 + aux streams + 4 half2 UVs.",
    streams=(
        _stream(0, 12, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        _stream(1, 8,
                _e(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0),
                _e(SEMANTIC_NORMAL, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 4)),
        _stream(4, 16,
                _e(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0),
                _e(SEMANTIC_TEXCOORD, 1, "DXGI_FORMAT_R16G16_FLOAT", 4, 4),
                _e(SEMANTIC_TEXCOORD, 2, "DXGI_FORMAT_R16G16_FLOAT", 4, 8),
                _e(SEMANTIC_TEXCOORD, 3, "DXGI_FORMAT_R16G16_FLOAT", 4, 12)),
    ),
)

#: Variant of UE5 static where UV stream is 8 bytes (only one half2 UV pair).
#: Observed on smaller static props.
UE5_STATIC_UV8 = VertexFactory(
    name="UE5.StaticUV8",
    engine="UE5",
    category="static",
    notes="UE5 static mesh with single half2 UV stream.",
    streams=(
        _stream(0, 12, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        _stream(1, 8,
                _e(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0),
                _e(SEMANTIC_NORMAL, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 4)),
        _stream(4, 8,
                _e(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0)),
    ),
)

#: Variant of UE5 static where TBN is in slot1 stride=4 (tangent only, no normal
#: stream — e.g. shadow/depth passes that don't read normals).
UE5_STATIC_TANGENT_ONLY = VertexFactory(
    name="UE5.StaticTangentOnly",
    engine="UE5",
    category="static",
    notes="UE5 static path with 4-byte TANGENT stream, no NORMAL stream.",
    streams=(
        _stream(0, 12, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        _stream(1, 4, _e(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0)),
        _stream(4, 8, _e(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0)),
    ),
)

#: GPU-preskinning skinned mesh layout where POSITION is in slot0 stride=12 (already
#: skinned) and skin weights/indices have been consumed by the CS before the VS.
#: Same stream geometry as StaticFull but tagged gpu_preskinning.
UE5_GPU_SKINNED_FULL = VertexFactory(
    name="UE5.GPUSkinnedFull",
    engine="UE5",
    category="gpu_preskinning",
    notes="GPU pre-skinned mesh with pos+TBN+4 UVs (post-skin pose).",
    streams=UE5_STATIC_FULL.streams,
)

#: GPU-pre-skinning variant where the position stream was written by the CS as
#: 32-byte vertices (common for Eve's body/hair which use an expanded format).
#: Until we decode the exact layout, we treat slot0 stride=32 as a Position stream
#: whose first 12 bytes are float3 POSITION and the rest are unused/aux.
UE5_GPU_SKINNED_EXPANDED = VertexFactory(
    name="UE5.GPUSkinnedExpanded",
    engine="UE5",
    category="gpu_preskinning",
    notes="GPU pre-skinned: slot0 stride=32 (first 12b = post-skin position).",
    streams=(
        _stream(0, 32, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
        _stream(1, 8,
                _e(SEMANTIC_TANGENT, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 0),
                _e(SEMANTIC_NORMAL, 0, "DXGI_FORMAT_R8G8B8A8_SNORM", 1, 4)),
        _stream(4, 16,
                _e(SEMANTIC_TEXCOORD, 0, "DXGI_FORMAT_R16G16_FLOAT", 4, 0),
                _e(SEMANTIC_TEXCOORD, 1, "DXGI_FORMAT_R16G16_FLOAT", 4, 4),
                _e(SEMANTIC_TEXCOORD, 2, "DXGI_FORMAT_R16G16_FLOAT", 4, 8),
                _e(SEMANTIC_TEXCOORD, 3, "DXGI_FORMAT_R16G16_FLOAT", 4, 12)),
    ),
)

#: Generic fallback: slot 0 stride >= 12 with POSITION float3 at offset 0 (the
#: remaining bytes in the vertex are ignored). This covers odd expanded formats
#: and debug/helper draws that only expose position data.
GENERIC_POSITION_ONLY_S12 = VertexFactory(
    name="Generic.PositionOnly",
    engine="generic",
    category="fallback",
    notes="Fallback: slot0 stride 12 -> POSITION float3.",
    streams=(
        _stream(0, 12, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
    ),
)

GENERIC_POSITION_ONLY_S16 = VertexFactory(
    name="Generic.PositionOnlyS16",
    engine="generic",
    category="fallback",
    notes="Fallback: slot0 stride 16, POSITION float3 at offset 0.",
    streams=(
        _stream(0, 16, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
    ),
)

GENERIC_POSITION_ONLY_S32 = VertexFactory(
    name="Generic.PositionOnlyS32",
    engine="generic",
    category="fallback",
    notes="Fallback: slot0 stride 32, POSITION float3 at offset 0.",
    streams=(
        _stream(0, 32, _e(SEMANTIC_POSITION, 0, "DXGI_FORMAT_R32G32B32_FLOAT", 0, 0)),
    ),
)


# Order matters when scores tie: more specific factories first.
FACTORIES: Tuple[VertexFactory, ...] = (
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


def match_factory(slot_strides: Dict[int, int], ib_format: Optional[str]) -> Optional[VertexFactory]:
    """Pick the best matching factory for a draw given its VB slot->stride map.

    Returns the factory with the highest match score, or None if no factory
    fits (which only happens if there is no stride-12 position stream).
    """
    best_score = 0
    best_factory: Optional[VertexFactory] = None
    for factory in FACTORIES:
        score = factory.score_for_draw(slot_strides, ib_format)
        if score > best_score:
            best_score = score
            best_factory = factory
    return best_factory
