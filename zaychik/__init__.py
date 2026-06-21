# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

from __future__ import annotations

import os
import re
import struct
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple

import bpy
from mathutils import Matrix
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import AddonPreferences, Operator, Panel, PropertyGroup, UIList


bl_info = {
    "name": "Zaychik DX12 Dump Importer",
    "author": "OpenAI",
    "description": "Analyze a DX12 frame dump log.txt and try importing meshes into Blender",
    "blender": (4, 2, 0),
    "version": (0, 1, 0),
    "location": "View3D > Sidebar > Zaychik",
    "warning": "Early prototype importer for DX12 frame dumps",
    "category": "Import-Export",
}


CALL_DRAW_RE = re.compile(
    r"call\.draw function=draw_indexed event=(?P<event>\d+).*?"
    r"vs=(?P<vs>[0-9a-f-]+).*?"
    r"topology=(?P<topology>[A-Z0-9_]+).*?"
    r"index_count=(?P<index_count>\d+).*?"
    r"start_vertex=(?P<start_vertex>-?\d+).*?"
    r"start_index=(?P<start_index>-?\d+).*?"
    r"base_vertex=(?P<base_vertex>-?\d+).*?"
    r"instance_count=(?P<instance_count>\d+)"
)

CALL_DRAW_SIMPLE_RE = re.compile(
    r"call\.draw function=draw_indexed event=(?P<event>\d+)"
)

BIND_IA_RE = re.compile(
    r"bind\.ia event=(?P<event>\d+).*?"
    r"role=(?P<role>VB|IB).*?"
    r"slot=(?P<slot>\d+).*?"
    r"bytes=(?P<bytes>\d+).*?"
    r"stride=(?P<stride>\d+).*?"
    r"fmt=(?P<fmt>\d+).*?"
    r"fmt_name=(?P<fmt_name>[A-Z0-9_]+).*?"
    r"(?:skin_source=(?P<skin_source>[a-z_]+).*?)?"
    r"file=(?P<file>deduped\\[^ ]+)"
)

BIND_RESOURCE_RE = re.compile(
    r"bind\.resource event=(?P<event>\d+).*?"
    r"bind=(?P<bind>[a-z_]+).*?"
    r"kind=(?P<kind>[A-Z]+).*?"
    r"bytes=(?P<bytes>\d+).*?"
    r"file=(?P<file>deduped\\[^ ]+)"
)

POSITION_LIKE_STRIDES = {12}
UV_LIKE_STRIDES = {8, 16}


@dataclass
class VertexBinding:
    slot: int
    bytes: int
    stride: int
    fmt: int
    fmt_name: str
    skin_source: str
    relative_path: str


@dataclass
class IndexBinding:
    bytes: int
    fmt: int
    fmt_name: str
    relative_path: str


@dataclass
class ConstantBufferBinding:
    bind_space: str
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


class ZAYCHIK_PG_frameanalysis_item(PropertyGroup):
    name: StringProperty(name="Name")
    path: StringProperty(name="Path")


def normalize_path(path: str) -> str:
    return os.path.normpath(path)


def read_binary_file(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def iter_log_lines(path: str) -> Iterable[str]:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            yield line.strip()


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
        if not bind_match:
            resource_match = BIND_RESOURCE_RE.search(line)
            if not resource_match:
                continue

            draw = draws.get(int(resource_match.group("event")))
            if draw is None or resource_match.group("kind") != "CBV":
                continue
            draw.constant_buffers.append(
                ConstantBufferBinding(
                    bind_space=resource_match.group("bind"),
                    bytes=int(resource_match.group("bytes")),
                    relative_path=resource_match.group("file"),
                )
            )
            continue

        event = int(bind_match.group("event"))
        draw = draws.get(event)
        if draw is None:
            simple_match = CALL_DRAW_SIMPLE_RE.search(line)
            if simple_match is None:
                continue

        role = bind_match.group("role")
        relative_path = bind_match.group("file")
        if role == "VB":
            binding = VertexBinding(
                slot=int(bind_match.group("slot")),
                bytes=int(bind_match.group("bytes")),
                stride=int(bind_match.group("stride")),
                fmt=int(bind_match.group("fmt")),
                fmt_name=bind_match.group("fmt_name"),
                skin_source=bind_match.group("skin_source") or "unknown",
                relative_path=relative_path,
            )
            if draw is not None:
                draw.vertex_bindings[binding.slot] = binding
                if binding.skin_source != "not_applicable":
                    if draw.skin_source == "unknown" or binding.skin_source == "gpu_preskinning":
                        draw.skin_source = binding.skin_source
        elif role == "IB" and draw is not None:
            draw.index_binding = IndexBinding(
                bytes=int(bind_match.group("bytes")),
                fmt=int(bind_match.group("fmt")),
                fmt_name=bind_match.group("fmt_name"),
                relative_path=relative_path,
            )

    return sorted(draws.values(), key=lambda item: item.event)


def resolve_binding_path(root_dir: str, relative_path: str) -> str:
    fixed_relative = relative_path.replace("\\", os.sep)
    return normalize_path(os.path.join(root_dir, fixed_relative))


def choose_position_binding(draw: DrawCall) -> Optional[VertexBinding]:
    candidates = [
        binding
        for binding in draw.vertex_bindings.values()
        if binding.stride in POSITION_LIKE_STRIDES and binding.bytes >= binding.stride * 3
    ]
    if not candidates:
        return None
    return sorted(candidates, key=lambda binding: (binding.slot, -binding.bytes))[0]


def choose_uv_binding(draw: DrawCall, vertex_count: int) -> Optional[VertexBinding]:
    candidates = []
    for binding in draw.vertex_bindings.values():
        if binding.stride not in UV_LIKE_STRIDES:
            continue
        if binding.bytes < vertex_count * 4:
            continue
        candidates.append(binding)
    if not candidates:
        return None
    return sorted(candidates, key=lambda binding: (binding.slot, binding.stride))[0]


def read_positions(path: str, max_vertices: int) -> List[Tuple[float, float, float]]:
    data = read_binary_file(path)
    vertex_count = min(len(data) // 12, max_vertices)
    positions = []
    for index in range(vertex_count):
        offset = index * 12
        positions.append(struct.unpack_from("<3f", data, offset))
    return positions


def decode_half(value: bytes) -> float:
    return struct.unpack("<e", value)[0]


def read_uvs(path: str, stride: int, vertex_count: int) -> Optional[List[Tuple[float, float]]]:
    data = read_binary_file(path)
    if stride < 4:
        return None

    available = min(len(data) // stride, vertex_count)
    uvs: List[Tuple[float, float]] = []

    if stride >= 8:
        try:
            for index in range(available):
                offset = index * stride
                u, v = struct.unpack_from("<2f", data, offset)
                if abs(u) > 100000 or abs(v) > 100000:
                    raise ValueError
                uvs.append((u, v))
            return uvs
        except (struct.error, ValueError):
            uvs.clear()

    if stride >= 4:
        try:
            for index in range(available):
                offset = index * stride
                u = decode_half(data[offset : offset + 2])
                v = decode_half(data[offset + 2 : offset + 4])
                uvs.append((u, v))
            return uvs
        except (struct.error, ValueError):
            return None

    return None


def read_indices(path: str, fmt_name: str, start_index: int, index_count: int) -> List[int]:
    data = read_binary_file(path)
    if fmt_name == "DXGI_FORMAT_R16_UINT":
        stride = 2
        unpack_format = "<H"
    elif fmt_name == "DXGI_FORMAT_R32_UINT":
        stride = 4
        unpack_format = "<I"
    else:
        raise ValueError(f"Unsupported index format: {fmt_name}")

    start_offset = start_index * stride
    end_offset = start_offset + (index_count * stride)
    if end_offset > len(data):
        raise ValueError("Index buffer does not contain enough data for this draw")

    indices: List[int] = []
    for offset in range(start_offset, end_offset, stride):
        indices.append(struct.unpack_from(unpack_format, data, offset)[0])
    return indices


def iter_float4x4(data: bytes) -> Iterable[Tuple[int, Tuple[float, ...]]]:
    float_count = len(data) // 4
    if float_count < 16:
        return
    for float_offset in range(0, float_count - 15, 4):
        byte_offset = float_offset * 4
        yield byte_offset, struct.unpack_from("<16f", data, byte_offset)


def finite_values(values: Tuple[float, ...]) -> bool:
    return all(value == value and abs(value) < 1.0e12 for value in values)


def score_world_matrix(matrix: Matrix) -> float:
    translation = matrix.to_translation()
    translation_len = translation.length
    if translation_len > 1.0e7:
        return -1.0

    basis_lengths = [matrix.col[index].to_3d().length for index in range(3)]
    if any(length < 0.0001 or length > 10000.0 for length in basis_lengths):
        return -1.0

    det = matrix.to_3x3().determinant()
    if abs(det) < 1.0e-8:
        return -1.0

    score = 0.0
    if translation_len > 0.001:
        score += 100.0
    score += min(translation_len, 100000.0) / 100000.0
    score -= sum(abs(length - 1.0) for length in basis_lengths)
    return score


def matrix_from_float4x4(values: Tuple[float, ...], row_vector_layout: bool) -> Matrix:
    rows = [values[index : index + 4] for index in range(0, 16, 4)]
    matrix = Matrix(rows)
    if row_vector_layout:
        matrix.transpose()
    return matrix


def find_world_matrix_in_buffer(path: str) -> Optional[Matrix]:
    data = read_binary_file(path)
    best_score = -1.0
    best_matrix: Optional[Matrix] = None

    for _offset, values in iter_float4x4(data):
        if not finite_values(values):
            continue
        for row_vector_layout in (False, True):
            matrix = matrix_from_float4x4(values, row_vector_layout)
            score = score_world_matrix(matrix)
            if score > best_score:
                best_score = score
                best_matrix = matrix

    return best_matrix if best_score >= 0.0 else None


def find_draw_world_matrix(dump_dir: str, draw: DrawCall) -> Optional[Matrix]:
    for binding in draw.constant_buffers:
        if binding.bind_space != "graphics_cbv_srv_uav":
            continue
        cbv_path = resolve_binding_path(dump_dir, binding.relative_path)
        if not os.path.isfile(cbv_path):
            continue
        matrix = find_world_matrix_in_buffer(cbv_path)
        if matrix is not None:
            return matrix
    return None


def build_faces(indices: List[int], base_vertex: int, vertex_count: int) -> List[Tuple[int, int, int]]:
    faces: List[Tuple[int, int, int]] = []
    usable = len(indices) - (len(indices) % 3)
    for offset in range(0, usable, 3):
        a = indices[offset] + base_vertex
        b = indices[offset + 1] + base_vertex
        c = indices[offset + 2] + base_vertex
        if a == b or b == c or a == c:
            continue
        if min(a, b, c) < 0:
            continue
        if max(a, b, c) >= vertex_count:
            continue
        faces.append((a, b, c))
    return faces


def apply_uvs(mesh: bpy.types.Mesh, uvs: List[Tuple[float, float]], faces: List[Tuple[int, int, int]]) -> None:
    if not uvs or not faces:
        return

    uv_layer = mesh.uv_layers.new(name="UVMap")
    loop_index = 0
    for face in faces:
        for vertex_index in face:
            if vertex_index < len(uvs):
                u, v = uvs[vertex_index]
                uv_layer.data[loop_index].uv = (u, 1.0 - v)
            loop_index += 1


def create_mesh_object(
    context: bpy.types.Context,
    name: str,
    positions: List[Tuple[float, float, float]],
    faces: List[Tuple[int, int, int]],
    uvs: Optional[List[Tuple[float, float]]],
    collection_name: Optional[str],
    world_matrix: Optional[Matrix],
) -> bpy.types.Object:
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(positions, [], faces)
    mesh.update()

    if uvs:
        apply_uvs(mesh, uvs, faces)

    obj = bpy.data.objects.new(name, mesh)
    if collection_name:
        collection = bpy.data.collections.get(collection_name)
        if collection is None:
            collection = bpy.data.collections.new(collection_name)
            context.scene.collection.children.link(collection)
        collection.objects.link(obj)
    else:
        context.collection.objects.link(obj)
    if world_matrix is not None:
        obj.matrix_world = world_matrix
    return obj


def import_draw_call(context: bpy.types.Context, dump_dir: str, draw: DrawCall) -> Tuple[bool, str]:
    if draw.topology != "TRIANGLELIST":
        return False, f"event {draw.event}: unsupported topology {draw.topology}"
    if draw.index_binding is None:
        return False, f"event {draw.event}: missing index buffer"

    position_binding = choose_position_binding(draw)
    if position_binding is None:
        return False, f"event {draw.event}: no position-like vertex buffer found"

    position_path = resolve_binding_path(dump_dir, position_binding.relative_path)
    index_path = resolve_binding_path(dump_dir, draw.index_binding.relative_path)
    if not os.path.isfile(position_path):
        return False, f"event {draw.event}: missing position buffer file"
    if not os.path.isfile(index_path):
        return False, f"event {draw.event}: missing index buffer file"

    indices = read_indices(index_path, draw.index_binding.fmt_name, draw.start_index, draw.index_count)
    if not indices:
        return False, f"event {draw.event}: empty index list"

    max_index = max(indices) + max(draw.base_vertex, 0) + 1
    positions = read_positions(position_path, max_index)
    if not positions:
        return False, f"event {draw.event}: empty position data"

    faces = build_faces(indices, draw.base_vertex, len(positions))
    if not faces:
        return False, f"event {draw.event}: no valid triangle faces"

    uv_data: Optional[List[Tuple[float, float]]] = None
    uv_binding = choose_uv_binding(draw, len(positions))
    if uv_binding is not None:
        uv_path = resolve_binding_path(dump_dir, uv_binding.relative_path)
        if os.path.isfile(uv_path):
            uv_data = read_uvs(uv_path, uv_binding.stride, len(positions))

    object_name = f"Dump_{draw.event}_{draw.skin_source}_{draw.vs[:8]}"
    collection_name = {
        "gpu_preskinning": "GPU-PreSkinning",
        "cpu_preskinning": "CPU-PreSkinning",
    }.get(draw.skin_source, "Unknown-PreSkinning")
    world_matrix = find_draw_world_matrix(dump_dir, draw)
    create_mesh_object(context, object_name, positions, faces, uv_data, collection_name, world_matrix)
    return True, f"Imported {object_name}"


class ZAYCHIK_PG_settings(PropertyGroup):
    max_imports: IntProperty(
        name="Max Imports",
        description="Maximum number of draw calls to try importing in one run",
        default=25,
        min=1,
        max=1000,
    )
    import_all_matching: BoolProperty(
        name="Import All Matching",
        description="Import every matching draw up to Max Imports instead of stopping after the first success",
        default=True,
    )
    skin_source_filter: EnumProperty(
        name="Skin Source",
        description="Filter draw calls by detected pre-skinning source",
        items=(
            ("all", "All", "Import all detected draw calls"),
            ("gpu_preskinning", "Only GPU-PreSkinning", "Import draw calls whose VB was produced by an earlier UAV write"),
            ("cpu_preskinning", "Only CPU-PreSkinning", "Import draw calls with direct IA vertex data"),
        ),
        default="all",
    )
    last_status: StringProperty(
        name="Status",
        description="Latest importer status",
        default="Ready",
    )
    frameanalysis_items: CollectionProperty(type=ZAYCHIK_PG_frameanalysis_item)
    frameanalysis_index: IntProperty(name="FrameAnalysis Index", default=0)


class ZAYCHIK_AP_preferences(AddonPreferences):
    bl_idname = __package__ or __name__

    dump_root_directory: StringProperty(
        name="Win64 Directory",
        description="Root directory containing FrameAnalysis-* folders",
        subtype="DIR_PATH",
    )
    selected_frameanalysis_name: StringProperty(
        name="Selected FrameAnalysis",
        description="Last selected FrameAnalysis directory name",
        default="",
    )


def get_addon_preferences(context: bpy.types.Context) -> ZAYCHIK_AP_preferences:
    addon = context.preferences.addons.get(__package__ or __name__)
    if addon is None:
        raise RuntimeError("Zaychik add-on preferences are unavailable")
    return addon.preferences


def scan_frameanalysis_directories(root_dir: str) -> List[Tuple[str, str]]:
    if not root_dir or not os.path.isdir(root_dir):
        return []

    entries: List[Tuple[str, str]] = []
    for entry in os.scandir(root_dir):
        if not entry.is_dir():
            continue
        if not entry.name.startswith("FrameAnalysis-"):
            continue
        entries.append((entry.name, normalize_path(entry.path)))

    entries.sort(key=lambda item: item[0], reverse=True)
    return entries


def refresh_frameanalysis_items(context: bpy.types.Context) -> int:
    settings = context.scene.zaychik_settings
    preferences = get_addon_preferences(context)
    root_dir = normalize_path(bpy.path.abspath(preferences.dump_root_directory).strip())
    selected_name = preferences.selected_frameanalysis_name

    settings.frameanalysis_items.clear()

    directories = scan_frameanalysis_directories(root_dir)
    selected_index = 0
    for index, (name, path) in enumerate(directories):
        item = settings.frameanalysis_items.add()
        item.name = name
        item.path = path
        if name == selected_name:
            selected_index = index

    if settings.frameanalysis_items:
        settings.frameanalysis_index = min(selected_index, len(settings.frameanalysis_items) - 1)
        preferences.selected_frameanalysis_name = settings.frameanalysis_items[
            settings.frameanalysis_index
        ].name
    else:
        settings.frameanalysis_index = 0
        preferences.selected_frameanalysis_name = ""

    return len(settings.frameanalysis_items)


def get_selected_frameanalysis_path(context: bpy.types.Context) -> Optional[str]:
    settings = context.scene.zaychik_settings
    if not settings.frameanalysis_items:
        return None
    if settings.frameanalysis_index < 0 or settings.frameanalysis_index >= len(
        settings.frameanalysis_items
    ):
        return None
    preferences = get_addon_preferences(context)
    item = settings.frameanalysis_items[settings.frameanalysis_index]
    preferences.selected_frameanalysis_name = item.name
    return item.path


class ZAYCHIK_OT_refresh_frameanalysis_list(Operator):
    bl_idname = "zaychik.refresh_frameanalysis_list"
    bl_label = "Refresh FrameAnalysis List"
    bl_description = "Scan the selected Win64 directory and list all FrameAnalysis folders"

    def execute(self, context: bpy.types.Context) -> set[str]:
        preferences = get_addon_preferences(context)
        root_dir = normalize_path(bpy.path.abspath(preferences.dump_root_directory).strip())
        if not root_dir:
            self.report({"ERROR"}, "Please select the Win64 directory first")
            return {"CANCELLED"}
        if not os.path.isdir(root_dir):
            self.report({"ERROR"}, "Selected Win64 directory does not exist")
            return {"CANCELLED"}

        count = refresh_frameanalysis_items(context)
        context.scene.zaychik_settings.last_status = f"Found {count} FrameAnalysis folder(s)"
        self.report({"INFO"}, context.scene.zaychik_settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_UL_frameanalysis_list(UIList):
    bl_idname = "ZAYCHIK_UL_frameanalysis_list"

    def draw_item(
        self,
        context: bpy.types.Context,
        layout: bpy.types.UILayout,
        data: bpy.types.ID,
        item: ZAYCHIK_PG_frameanalysis_item,
        icon: int,
        active_data: bpy.types.ID,
        active_propname: str,
        index: int,
        flt_flag: int,
    ) -> None:
        del context, data, icon, active_data, active_propname, index, flt_flag
        layout.label(text=item.name, icon="FILE_FOLDER")


class ZAYCHIK_OT_import_dx12_dump(Operator):
    bl_idname = "zaychik.import_dx12_dump"
    bl_label = "Analyze log.txt And Import"
    bl_description = "Analyze log.txt in the selected dump directory and try importing model meshes"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context: bpy.types.Context) -> set[str]:
        settings = context.scene.zaychik_settings
        dump_dir = get_selected_frameanalysis_path(context)
        if not dump_dir:
            self.report({"ERROR"}, "Please select a FrameAnalysis directory from the list")
            return {"CANCELLED"}

        dump_dir = normalize_path(bpy.path.abspath(dump_dir).strip())
        log_path = os.path.join(dump_dir, "log.txt")
        if not os.path.isdir(dump_dir):
            self.report({"ERROR"}, "Dump directory does not exist")
            return {"CANCELLED"}
        if not os.path.isfile(log_path):
            self.report({"ERROR"}, "Selected directory does not contain log.txt")
            return {"CANCELLED"}

        try:
            draws = parse_draw_calls(log_path)
        except Exception as exc:  # pragma: no cover - Blender runtime path
            self.report({"ERROR"}, f"Failed to parse log.txt: {exc}")
            settings.last_status = "Parse failed"
            return {"CANCELLED"}

        matching_draws = [draw for draw in draws if draw.index_binding and draw.vertex_bindings]
        if settings.skin_source_filter != "all":
            matching_draws = [
                draw for draw in matching_draws if draw.skin_source == settings.skin_source_filter
            ]
        if not matching_draws:
            self.report({"WARNING"}, "No usable indexed draw calls were found")
            settings.last_status = "No usable draw calls"
            return {"CANCELLED"}

        success_count = 0
        messages: List[str] = []
        limit = min(settings.max_imports, len(matching_draws))
        for draw in matching_draws[:limit]:
            try:
                ok, message = import_draw_call(context, dump_dir, draw)
            except Exception as exc:  # pragma: no cover - Blender runtime path
                ok = False
                message = f"event {draw.event}: {exc}"

            if ok:
                success_count += 1
                messages.append(message)
                if not settings.import_all_matching:
                    break

        if success_count == 0:
            preview = messages[0] if messages else "No draw could be imported"
            self.report({"WARNING"}, preview)
            settings.last_status = "Import attempt finished with 0 success"
            return {"CANCELLED"}

        settings.last_status = f"Imported {success_count} mesh object(s)"
        self.report({"INFO"}, settings.last_status)
        return {"FINISHED"}


class ZAYCHIK_PT_sidebar(Panel):
    bl_label = "Zaychik"
    bl_idname = "ZAYCHIK_PT_sidebar"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Zaychik"

    def draw(self, context: bpy.types.Context) -> None:
        layout = self.layout
        settings = context.scene.zaychik_settings
        preferences = get_addon_preferences(context)

        layout.prop(preferences, "dump_root_directory")
        layout.operator(ZAYCHIK_OT_refresh_frameanalysis_list.bl_idname, icon="FILE_REFRESH")
        layout.template_list(
            ZAYCHIK_UL_frameanalysis_list.bl_idname,
            "",
            settings,
            "frameanalysis_items",
            settings,
            "frameanalysis_index",
            rows=8,
        )
        layout.prop(settings, "max_imports")
        layout.prop(settings, "import_all_matching")
        layout.prop(settings, "skin_source_filter")
        layout.operator(ZAYCHIK_OT_import_dx12_dump.bl_idname, icon="IMPORT")

        box = layout.box()
        box.label(text="Status")
        box.label(text=settings.last_status)

        selected_path = get_selected_frameanalysis_path(context)
        if selected_path:
            box.label(text=os.path.basename(selected_path))


CLASSES = (
    ZAYCHIK_PG_frameanalysis_item,
    ZAYCHIK_PG_settings,
    ZAYCHIK_AP_preferences,
    ZAYCHIK_OT_refresh_frameanalysis_list,
    ZAYCHIK_OT_import_dx12_dump,
    ZAYCHIK_UL_frameanalysis_list,
    ZAYCHIK_PT_sidebar,
)


def register() -> None:
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.zaychik_settings = PointerProperty(type=ZAYCHIK_PG_settings)

    try:
        for window in bpy.context.window_manager.windows:
            screen = window.screen
            if screen is None:
                continue
            for area in screen.areas:
                if area.type != "VIEW_3D":
                    continue
                with bpy.context.temp_override(window=window, area=area):
                    refresh_frameanalysis_items(bpy.context)
                return
    except Exception:
        pass


def unregister() -> None:
    del bpy.types.Scene.zaychik_settings
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
