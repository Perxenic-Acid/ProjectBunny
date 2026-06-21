from __future__ import annotations

import bpy
from bpy.props import PointerProperty

from .common.config import load_config
from .common.properties import CLASSES as PROPERTY_CLASSES
from .common.properties import ZAYCHIK_PG_settings
from .ui.operators import CLASSES as OPERATOR_CLASSES
from .ui.operators import refresh_frameanalysis_items
from .ui.panel import CLASSES as PANEL_CLASSES


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


CLASSES = (
    *PROPERTY_CLASSES,
    *OPERATOR_CLASSES,
    *PANEL_CLASSES,
)


def register() -> None:
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.zaychik_settings = PointerProperty(type=ZAYCHIK_PG_settings)

    try:
        settings = bpy.context.scene.zaychik_settings
        config = load_config()
        settings.dump_root_directory = config.get("dump_root_directory", "")
        settings.selected_frameanalysis_name = config.get("selected_frameanalysis_name", "")
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
