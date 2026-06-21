from __future__ import annotations

import os

import bpy
from bpy.types import Panel, UIList

from ..common.properties import ZAYCHIK_PG_frameanalysis_item
from .operators import (
    FrameAnalysisUI,
    ZAYCHIK_OT_import_dx12_dump,
    ZAYCHIK_OT_refresh_frameanalysis_list,
)


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


class ZAYCHIK_PT_sidebar(Panel):
    bl_label = "Zaychik"
    bl_idname = "ZAYCHIK_PT_sidebar"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Zaychik"

    def draw(self, context: bpy.types.Context) -> None:
        layout = self.layout
        settings = context.scene.zaychik_settings

        layout.prop(settings, "dump_root_directory")
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
        layout.prop(settings, "apply_world_matrices")
        if settings.apply_world_matrices:
            layout.prop(settings, "world_matrix_scale")
        layout.prop(settings, "skin_source_filter")
        layout.prop(settings, "vertex_layout_preset")
        layout.operator(ZAYCHIK_OT_import_dx12_dump.bl_idname, icon="IMPORT")

        box = layout.box()
        box.label(text="Status")
        box.label(text=settings.last_status)

        selected_path = FrameAnalysisUI.selected_path(context)
        if selected_path:
            box.label(text=os.path.basename(selected_path))


CLASSES = (
    ZAYCHIK_UL_frameanalysis_list,
    ZAYCHIK_PT_sidebar,
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
