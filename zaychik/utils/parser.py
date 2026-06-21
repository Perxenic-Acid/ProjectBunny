from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional


# Key=value pairs in the log can be nested inside longer names (e.g.
# "buffer_view_bytes=0 ... bytes=4096" or "ib_bytes=0 ... bytes=1152").
# The _TOPLEVEL lookbehind ensures we match a top-level key, not the tail
# of a longer identifier.
_TOPLEVEL = r"(?<![A-Za-z0-9_])"


CALL_DRAW_RE = re.compile(
    r"call\.draw function=draw_indexed event=(?P<event>\d+).*?"
    r"vs=(?P<vs>[0-9a-f-]+).*?"
    r"topology=(?P<topology>[A-Z0-9_]+).*?"
    + _TOPLEVEL + r"index_count=(?P<index_count>\d+).*?"
    + _TOPLEVEL + r"start_vertex=(?P<start_vertex>-?\d+).*?"
    + _TOPLEVEL + r"start_index=(?P<start_index>-?\d+).*?"
    + _TOPLEVEL + r"base_vertex=(?P<base_vertex>-?\d+).*?"
    + _TOPLEVEL + r"instance_count=(?P<instance_count>\d+)"
)

CALL_DRAW_SIMPLE_RE = re.compile(
    r"call\.draw function=draw_indexed event=(?P<event>\d+)"
)

BIND_IA_RE = re.compile(
    r"bind\.ia event=(?P<event>\d+).*?"
    r"role=(?P<role>VB|IB).*?"
    r"slot=(?P<slot>\d+).*?"
    + _TOPLEVEL + r"gpu=(?P<gpu>0x[0-9a-fA-F]+).*?"
    + _TOPLEVEL + r"offset=(?P<offset>\d+).*?"
    + _TOPLEVEL + r"bytes=(?P<bytes>\d+).*?"
    r"stride=(?P<stride>\d+).*?"
    r"fmt=(?P<fmt>\d+).*?"
    r"fmt_name=(?P<fmt_name>[A-Z0-9_]+).*?"
    r"(?:" + _TOPLEVEL + r"skin_source=(?P<skin_source>[a-z_]+).*?)?"
    r"file=(?P<file>deduped\\[^ ]+)"
)

BIND_RESOURCE_RE = re.compile(
    r"bind\.resource event=(?P<event>\d+).*?"
    r"bind=(?P<bind>[a-z_]+).*?"
    r"kind=(?P<kind>[A-Z]+).*?"
    + _TOPLEVEL + r"root=(?P<root>\d+).*?"
    + _TOPLEVEL + r"reg=(?P<reg>\d+).*?"
    + _TOPLEVEL + r"bytes=(?P<bytes>\d+).*?"
    r"file=(?P<file>deduped\\[^ ]+)"
)


@dataclass
class VertexBinding:
    slot: int
    bytes: int
    stride: int
    fmt: int
    fmt_name: str
    skin_source: str
    relative_path: str
    gpu: int = 0
    offset: int = 0


@dataclass
class IndexBinding:
    bytes: int
    fmt: int
    fmt_name: str
    relative_path: str
    gpu: int = 0
    offset: int = 0


@dataclass
class ConstantBufferBinding:
    bind_space: str
    root_index: int
    reg: int
    bytes: int
    relative_path: str


@dataclass
class DrawCall:
    event: int
    vs: str
    topology: str
    index_count: int
    start_vertex: int
    start_index: int
    base_vertex: int
    instance_count: int
    skin_source: str = "unknown"
    vertex_bindings: Dict[int, VertexBinding] = field(default_factory=dict)
    index_binding: Optional[IndexBinding] = None
    constant_buffers: List[ConstantBufferBinding] = field(default_factory=list)


def iter_log_lines(path: str) -> Iterable[str]:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            yield line.strip()


def _to_int(value: str, base: int = 10) -> int:
    return int(value, base) if value else 0


def parse_draw_calls(log_path: str) -> List[DrawCall]:
    draws: Dict[int, DrawCall] = {}

    for line in iter_log_lines(log_path):
        draw_match = CALL_DRAW_RE.search(line)
        if draw_match:
            event = int(draw_match.group("event"))
            draws[event] = DrawCall(
                event=event,
                vs=draw_match.group("vs"),
                topology=draw_match.group("topology"),
                index_count=int(draw_match.group("index_count")),
                start_vertex=int(draw_match.group("start_vertex")),
                start_index=int(draw_match.group("start_index")),
                base_vertex=int(draw_match.group("base_vertex")),
                instance_count=int(draw_match.group("instance_count")),
            )
            continue

        bind_match = BIND_IA_RE.search(line)
        if bind_match:
            event = int(bind_match.group("event"))
            draw = draws.get(event)
            role = bind_match.group("role")
            relative_path = bind_match.group("file")
            gpu = _to_int(bind_match.group("gpu"), 16)
            offset = int(bind_match.group("offset"))
            if role == "VB":
                binding = VertexBinding(
                    slot=int(bind_match.group("slot")),
                    bytes=int(bind_match.group("bytes")),
                    stride=int(bind_match.group("stride")),
                    fmt=int(bind_match.group("fmt")),
                    fmt_name=bind_match.group("fmt_name"),
                    skin_source=bind_match.group("skin_source") or "unknown",
                    relative_path=relative_path,
                    gpu=gpu,
                    offset=offset,
                )
                if draw is not None:
                    draw.vertex_bindings[binding.slot] = binding
                    if binding.skin_source != "not_applicable":
                        if draw.skin_source == "unknown" or binding.skin_source == "gpu_preskinning":
                            draw.skin_source = binding.skin_source
            elif role == "IB":
                ib = IndexBinding(
                    bytes=int(bind_match.group("bytes")),
                    fmt=int(bind_match.group("fmt")),
                    fmt_name=bind_match.group("fmt_name"),
                    relative_path=relative_path,
                    gpu=gpu,
                    offset=offset,
                )
                if draw is not None:
                    draw.index_binding = ib
            continue

        resource_match = BIND_RESOURCE_RE.search(line)
        if not resource_match:
            continue

        draw = draws.get(int(resource_match.group("event")))
        if draw is None:
            continue
        if resource_match.group("kind") != "CBV":
            continue
        draw.constant_buffers.append(
            ConstantBufferBinding(
                bind_space=resource_match.group("bind"),
                root_index=int(resource_match.group("root")),
                reg=int(resource_match.group("reg")),
                bytes=int(resource_match.group("bytes")),
                relative_path=resource_match.group("file"),
            )
        )

    return sorted(draws.values(), key=lambda item: item.event)
