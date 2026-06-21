from __future__ import annotations

from bpy.props import BoolProperty, CollectionProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import PropertyGroup

from .config import on_config_property_changed


class ZAYCHIK_PG_frameanalysis_item(PropertyGroup):
    name: StringProperty(name="Name")
    path: StringProperty(name="Path")


class ZAYCHIK_PG_settings(PropertyGroup):
    dump_root_directory: StringProperty(
        name="Win64 Directory",
        description="Root directory containing FrameAnalysis-* folders",
        subtype="DIR_PATH",
        update=on_config_property_changed,
    )
    selected_frameanalysis_name: StringProperty(
        name="Selected FrameAnalysis",
        description="Last selected FrameAnalysis directory name",
        default="",
        update=on_config_property_changed,
    )
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


CLASSES = (
    ZAYCHIK_PG_frameanalysis_item,
    ZAYCHIK_PG_settings,
)

