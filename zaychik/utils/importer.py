from __future__ import annotations

import os
import struct
from typing import Iterable, List, Optional, Tuple

import bpy
from mathutils import Matrix

from .parser import DrawCall, VertexBinding
from .paths import resolve_binding_path


POSITION_LIKE_STRIDES = {12}
UV_LIKE_STRIDES = {8, 16}


def read_binary_file(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


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

