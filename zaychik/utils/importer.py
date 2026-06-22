"""Turn parsed DrawCall records into Blender mesh objects using strictly-typed
vertex/factory decoding.
"""
from __future__ import annotations

import os
import struct
from typing import Dict, List, Optional, Tuple

import bpy
from mathutils import Matrix, Vector

from .layouts import (
    SEMANTIC_NORMAL,
    SEMANTIC_POSITION,
    SEMANTIC_TEXCOORD,
    VertexElement,
    VertexFactory,
)
from .parser import DrawCall, VertexBinding
from .paths import Paths


class DrawImporter:
    """Imports a single :class:`DrawCall` into the Blender scene.

    All helpers are @staticmethods or @classmethods because the importer is
    stateless — each call to ``import_draw_call`` operates on the draw passed
    in and reads buffers fresh from disk.
    """

    # CBV bind-space labels we treat as "graphics root signature" (world
    # matrix candidates).
    _GRAPHICS_CBV_BIND_SPACES = frozenset({
        "graphics_cbv_srv_uav",
        "graphics_root",
        "graphics",
    })

    # ------------------------------------------------------------------
    # Binary IO
    # ------------------------------------------------------------------

    @staticmethod
    def _read_binary_file(path: str) -> bytes:
        with open(path, "rb") as handle:
            return handle.read()

    @classmethod
    def _resolve_path(cls, dump_dir: str, rel: str) -> str:
        return Paths.resolve_binding(dump_dir, rel)

    # ------------------------------------------------------------------
    # Index buffer
    # ------------------------------------------------------------------

    @classmethod
    def _read_indices(cls, data: bytes, fmt_name: str,
                      start_index: int, index_count: int) -> List[int]:
        from .formats import DxgiFormat
        fmt = DxgiFormat.get(fmt_name)
        if fmt is None or not fmt.is_integer or fmt.component_count != 1 or fmt.byte_width not in (2, 4):
            raise ValueError(f"Unsupported index format: {fmt_name}")
        stride = fmt.byte_width
        start_offset = start_index * stride
        end_offset = start_offset + index_count * stride
        if end_offset > len(data):
            raise ValueError(
                f"Index buffer short: need {end_offset} bytes, have {len(data)}"
            )
        indices: List[int] = []
        for offset in range(start_offset, end_offset, stride):
            (value,) = fmt.decode(data, offset)
            indices.append(int(value))
        return indices

    # ------------------------------------------------------------------
    # Vertex stream readers
    # ------------------------------------------------------------------

    @staticmethod
    def _decode_stream_element(data: bytes, element: VertexElement,
                               vertex_count: int, stream_stride: int) -> List[Tuple]:
        out: List[Tuple] = []
        fw = element.byte_width
        decode = element.format_info.decode
        for i in range(vertex_count):
            base = i * stream_stride + element.offset
            if base + fw > len(data):
                break
            out.append(decode(data, base))
        return out

    @staticmethod
    def _vertex_count_for_binding(data: bytes, binding: VertexBinding) -> int:
        if binding.stride <= 0:
            return 0
        return len(data) // binding.stride

    # ------------------------------------------------------------------
    # Factory selection
    # ------------------------------------------------------------------

    @staticmethod
    def select_factory(draw: DrawCall, preset: str = "auto") -> Optional[VertexFactory]:
        slot_info: Dict[int, Tuple[int, Optional[str]]] = {
            slot: (b.stride, b.fmt_name)
            for slot, b in draw.vertex_bindings.items()
            if b.stride > 0
        }
        ib_fmt = draw.index_binding.fmt_name if draw.index_binding else None
        return VertexFactory.match(slot_info, ib_fmt, preset)

    @staticmethod
    def _element_by_semantic(draw: DrawCall, factory: VertexFactory,
                             semantic: str, semantic_index: int = 0,
                             ) -> Optional[Tuple[VertexElement, VertexBinding]]:
        for stream in factory.streams:
            for element in stream.elements:
                if element.semantic == semantic and element.semantic_index == semantic_index:
                    binding = draw.vertex_bindings.get(element.slot)
                    if binding is not None:
                        return element, binding
        return None

    # ------------------------------------------------------------------
    # Face / UV / mesh helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _build_faces(indices: List[int], base_vertex: int,
                     vertex_count: int) -> List[Tuple[int, int, int]]:
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

    @staticmethod
    def _apply_uvs(mesh: bpy.types.Mesh, uvs: List[Tuple[float, float]],
                   faces: List[Tuple[int, int, int]]) -> None:
        if not uvs or not faces:
            return
        uv_layer = mesh.uv_layers.new(name="UVMap")
        loop_index = 0
        for face in faces:
            for vertex_index in face:
                if 0 <= vertex_index < len(uvs):
                    u, v = uvs[vertex_index]
                    uv_layer.data[loop_index].uv = (u, 1.0 - v)
                loop_index += 1

    # ------------------------------------------------------------------
    # World-matrix detection in CBVs
    # ------------------------------------------------------------------

    @classmethod
    def _is_graphics_cbv_binding(cls, bind_space: str) -> bool:
        if bind_space in cls._GRAPHICS_CBV_BIND_SPACES:
            return True
        return bind_space.startswith("graphics")

    @staticmethod
    def _iter_float4x4(data: bytes):
        float_count = len(data) // 4
        if float_count < 16:
            return
        for float_offset in range(0, float_count - 15, 4):
            byte_offset = float_offset * 4
            yield byte_offset, struct.unpack_from("<16f", data, byte_offset)

    @staticmethod
    def _finite_values(values) -> bool:
        return all(v == v and abs(v) < 1e12 for v in values)

    @staticmethod
    def _score_world_matrix(matrix: Matrix) -> float:
        translation = matrix.to_translation()
        translation_len = translation.length
        if translation_len > 10000.0:
            return -1.0
        basis_lengths = [matrix.col[i].to_3d().length for i in range(3)]
        if any(L < 1e-4 or L > 10000.0 for L in basis_lengths):
            return -1.0
        det = matrix.to_3x3().determinant()
        if abs(det) < 1e-8:
            return -1.0
        score = 0.0
        if translation_len > 0.001:
            score += 100.0
        score += min(translation_len, 10000.0) / 10000.0
        score -= sum(abs(L - 1.0) for L in basis_lengths)
        return score

    @staticmethod
    def _matrix_from_float4x4(values, row_vector_layout: bool) -> Matrix:
        rows = [list(values[i:i + 4]) for i in range(0, 16, 4)]
        m = Matrix(rows)
        if row_vector_layout:
            m.transpose()
        return m

    @classmethod
    def _score_matrix_in_buffer(cls, path: str) -> Optional[Tuple[float, Matrix]]:
        try:
            data = cls._read_binary_file(path)
        except OSError:
            return None
        best_score = -1.0
        best_matrix: Optional[Matrix] = None
        for _, values in cls._iter_float4x4(data):
            if not cls._finite_values(values):
                continue
            for row_vector_layout in (False, True):
                m = cls._matrix_from_float4x4(values, row_vector_layout)
                s = cls._score_world_matrix(m)
                if s > best_score:
                    best_score = s
                    best_matrix = m
        if best_matrix is None or best_score < 0.0:
            return None
        return best_score, best_matrix

    @classmethod
    def find_draw_world_matrix(cls, dump_dir: str, draw: DrawCall) -> Optional[Matrix]:
        best_score = -1.0
        best_matrix: Optional[Matrix] = None
        for binding in draw.constant_buffers:
            if not cls._is_graphics_cbv_binding(binding.bind_space):
                continue
            cbv_path = cls._resolve_path(dump_dir, binding.relative_path)
            if not os.path.isfile(cbv_path):
                continue
            scored = cls._score_matrix_in_buffer(cbv_path)
            if scored is None:
                continue
            s, m = scored
            if s > best_score:
                best_score = s
                best_matrix = m
        return best_matrix

    # ------------------------------------------------------------------
    # Mesh object creation
    # ------------------------------------------------------------------

    @classmethod
    def _create_mesh_object(cls,
                            context: bpy.types.Context,
                            name: str,
                            positions: List[Tuple[float, float, float]],
                            faces: List[Tuple[int, int, int]],
                            uvs: Optional[List[Tuple[float, float]]],
                            normals: Optional[List[Tuple[float, float, float]]],
                            collection_name: Optional[str],
                            world_matrix: Optional[Matrix]) -> bpy.types.Object:
        local_center: Optional[Vector] = None
        if world_matrix is None and positions:
            min_x = min(p[0] for p in positions)
            min_y = min(p[1] for p in positions)
            min_z = min(p[2] for p in positions)
            max_x = max(p[0] for p in positions)
            max_y = max(p[1] for p in positions)
            max_z = max(p[2] for p in positions)
            local_center = Vector((
                (min_x + max_x) * 0.5,
                (min_y + max_y) * 0.5,
                (min_z + max_z) * 0.5,
            ))
            positions = [
                (
                    p[0] - local_center.x,
                    p[1] - local_center.y,
                    p[2] - local_center.z,
                )
                for p in positions
            ]

        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(positions, [], faces)
        mesh.update()

        if normals and len(normals) == len(positions):
            mesh.shade_smooth()
            try:
                mesh.normals_split_custom_set_from_vertices([Vector(n) for n in normals])
            except Exception:
                pass

        if uvs:
            cls._apply_uvs(mesh, uvs, faces)

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

    # ------------------------------------------------------------------
    # Main entry point
    # ------------------------------------------------------------------

    @classmethod
    def import_draw_call(cls, context: bpy.types.Context, dump_dir: str,
                         draw: DrawCall,
                         apply_world_matrix: bool = False,
                         world_matrix_scale: float = 0.001,
                         vertex_layout_preset: str = "auto") -> Tuple[bool, str]:
        if draw.topology != "TRIANGLELIST":
            return False, f"event {draw.event}: unsupported topology {draw.topology}"
        if draw.index_binding is None:
            return False, f"event {draw.event}: missing index buffer"

        factory = cls.select_factory(draw, vertex_layout_preset)
        if factory is None:
            return False, f"event {draw.event}: no matching vertex layout"

        # Indices
        ib_path = cls._resolve_path(dump_dir, draw.index_binding.relative_path)
        if not os.path.isfile(ib_path):
            return False, f"event {draw.event}: missing IB file"
        ib_data = cls._read_binary_file(ib_path)
        try:
            indices = cls._read_indices(ib_data, draw.index_binding.fmt_name,
                                        draw.start_index, draw.index_count)
        except (ValueError, struct.error) as exc:
            return False, f"event {draw.event}: IB decode failed ({exc})"
        if not indices:
            return False, f"event {draw.event}: empty index list"

        # Position stream (every factory declares POSITION)
        pos_hit = cls._element_by_semantic(draw, factory, SEMANTIC_POSITION, 0)
        if pos_hit is None:
            return False, f"event {draw.event}: no POSITION element in matched factory"
        pos_element, pos_binding = pos_hit
        pos_path = cls._resolve_path(dump_dir, pos_binding.relative_path)
        if not os.path.isfile(pos_path):
            return False, f"event {draw.event}: missing position buffer file"
        pos_data = cls._read_binary_file(pos_path)
        positions_raw = cls._decode_stream_element(
            pos_data, pos_element,
            cls._vertex_count_for_binding(pos_data, pos_binding),
            pos_binding.stride,
        )
        positions = [(float(p[0]), float(p[1]), float(p[2])) for p in positions_raw]
        if not positions:
            return False, f"event {draw.event}: empty position data"

        # Faces
        max_index = max(indices) + max(draw.base_vertex, 0) + 1
        faces = cls._build_faces(indices, draw.base_vertex,
                                 min(len(positions), max(len(positions), max_index)))
        if not faces:
            return False, f"event {draw.event}: no valid triangle faces"

        # UVs (TEXCOORD0)
        uvs: Optional[List[Tuple[float, float]]] = None
        uv_hit = cls._element_by_semantic(draw, factory, SEMANTIC_TEXCOORD, 0)
        if uv_hit is not None:
            uv_element, uv_binding = uv_hit
            uv_path = cls._resolve_path(dump_dir, uv_binding.relative_path)
            if os.path.isfile(uv_path):
                uv_data = cls._read_binary_file(uv_path)
                raw = cls._decode_stream_element(
                    uv_data, uv_element,
                    cls._vertex_count_for_binding(uv_data, uv_binding),
                    uv_binding.stride,
                )
                if raw and len(raw[0]) >= 2:
                    uvs = []
                    for t in raw:
                        try:
                            uvs.append((float(t[0]), float(t[1])))
                        except (TypeError, ValueError):
                            uvs.append((0.0, 0.0))

        # Normals (NORMAL)
        normals: Optional[List[Tuple[float, float, float]]] = None
        nrm_hit = cls._element_by_semantic(draw, factory, SEMANTIC_NORMAL, 0)
        if nrm_hit is not None:
            nrm_element, nrm_binding = nrm_hit
            nrm_path = cls._resolve_path(dump_dir, nrm_binding.relative_path)
            if os.path.isfile(nrm_path):
                nrm_data = cls._read_binary_file(nrm_path)
                raw = cls._decode_stream_element(
                    nrm_data, nrm_element,
                    cls._vertex_count_for_binding(nrm_data, nrm_binding),
                    nrm_binding.stride,
                )
                if raw and len(raw[0]) >= 3:
                    normals = [(float(t[0]), float(t[1]), float(t[2])) for t in raw]

        object_name = f"Dump_{draw.event}_{draw.skin_source}_{draw.vs[:8]}"
        collection_name = {
            "gpu_preskinning": "GPU-PreSkinning",
            "cpu_preskinning": "CPU-PreSkinning",
        }.get(draw.skin_source, "Unknown-PreSkinning")
        world_matrix = cls.find_draw_world_matrix(dump_dir, draw) if apply_world_matrix else None
        if world_matrix is not None and world_matrix_scale != 1.0:
            scale = max(float(world_matrix_scale), 0.000001)
            world_matrix = Matrix.Diagonal((scale, scale, scale, 1.0)) @ world_matrix
        cls._create_mesh_object(context, object_name, positions, faces, uvs,
                                normals, collection_name, world_matrix)
        return True, (
            f"Imported {object_name} via {factory.name} "
            f"(verts={len(positions)}, faces={len(faces)})"
        )
