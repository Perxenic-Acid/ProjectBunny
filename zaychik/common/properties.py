from __future__ import annotations

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from bpy.types import PropertyGroup

from .config import ConfigManager


def on_frameanalysis_index_changed(self: PropertyGroup, context: bpy.types.Context) -> None:
    """Keep selected_frameanalysis_name in sync when the active index changes."""
    del context
    items = self.frameanalysis_items
    index = self.frameanalysis_index
    if 0 <= index < len(items):
        self.selected_frameanalysis_name = items[index].name
    else:
        self.selected_frameanalysis_name = ""
    ConfigManager.save_config(self)


class ZAYCHIK_PG_frameanalysis_item(PropertyGroup):
    name: StringProperty(name="Name")
    path: StringProperty(name="Path")


class ZAYCHIK_PG_settings(PropertyGroup):
    dump_root_directory: StringProperty(
        name="Migoto Directory",
        description="Root directory containing FrameAnalysis-* folders",
        subtype="DIR_PATH",
        update=ConfigManager.on_config_property_changed,
    ) # type: ignore
    selected_frameanalysis_name: StringProperty(
        name="Selected FrameAnalysis",
        description="Last selected FrameAnalysis directory name",
        default="",
        update=ConfigManager.on_config_property_changed,
    ) # type: ignore
    max_imports: IntProperty(
        name="Max Imports",
        description="Maximum number of draw calls to try importing in one run",
        default=100,
        min=1,
        max=1000,
    ) # type: ignore
    import_all_matching: BoolProperty(
        name="Import All Matching",
        description="Import every matching draw up to Max Imports instead of stopping after the first success",
        default=True,
    ) # type: ignore
    apply_world_matrices: BoolProperty(
        name="Apply CBV World Matrix",
        description="Apply a guessed world matrix from constant buffers; disable when imports appear far away or invisible",
        default=False,
    ) # type: ignore
    world_matrix_scale: FloatProperty(
        name="World Matrix Scale",
        description="Scale applied after the guessed world matrix; use this to convert game units to Blender units",
        default=0.001,
        min=0.000001,
        max=1000.0,
        precision=6,
    ) # type: ignore
    skin_source_filter: EnumProperty(
        name="Skin Source",
        description="Filter draw calls by detected pre-skinning source",
        items=(
            ("all", "All", "Import all detected draw calls"),
            ("gpu_preskinning", "Only GPU-PreSkinning", "Import draw calls whose VB was produced by an earlier UAV write"),
            ("cpu_preskinning", "Only CPU-PreSkinning", "Import draw calls with direct IA vertex data"),
        ),
        default="all",
    ) # type: ignore
    vertex_layout_preset: EnumProperty(
        name="Vertex Layout Preset",
        description="Prefer game-specific vertex buffer layout definitions when decoding draws",
        items=(
            ("auto", "Auto", "Try all known presets"),
            ("stellar_blade", "Stellar Blade", "Use Stellar Blade DX12 vertex layouts first"),
            ("ue5", "UE5 Generic", "Use generic UE5 vertex layouts"),
            ("generic", "Generic", "Use fallback position-only layouts"),
        ),
        default="stellar_blade",
    ) # type: ignore
    last_status: StringProperty(
        name="Status",
        description="Latest importer status",
        default="Ready",
    ) # type: ignore
    frameanalysis_items: CollectionProperty(type=ZAYCHIK_PG_frameanalysis_item) # type: ignore
    frameanalysis_index: IntProperty(
        name="FrameAnalysis Index",
        default=0,
        update=on_frameanalysis_index_changed,
    ) # type: ignore


class SettingsCallbacks:
    """Static helpers for PropertyGroup update callbacks.

    These live in their own class (instead of inside ZAYCHIK_PG_settings)
    because Blender's ``update=`` callback must be a plain callable and
    Blender re-instantiates PropertyGroup classes; binding to a module-level
    classmethod avoids re-registration issues.
    """

    @staticmethod
    def on_frameanalysis_index_changed(self: PropertyGroup, context: bpy.types.Context) -> None:
        on_frameanalysis_index_changed(self, context)


CLASSES = (
    ZAYCHIK_PG_frameanalysis_item,
    ZAYCHIK_PG_settings,
)


def register() -> None:
    for klass in CLASSES:
        bpy.utils.register_class(klass)


def unregister() -> None:
    for klass in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(klass)
        except (RuntimeError, ValueError):
            pass
