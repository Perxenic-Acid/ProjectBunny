from __future__ import annotations

import os
from typing import List

import bpy
from bpy.types import Operator

from ..common.config import save_config
from ..utils.importer import import_draw_call
from ..utils.parser import parse_draw_calls
from ..utils.paths import normalize_path, scan_frameanalysis_directories


def refresh_frameanalysis_items(context: bpy.types.Context) -> int:
    settings = context.scene.zaychik_settings
    root_dir = normalize_path(bpy.path.abspath(settings.dump_root_directory).strip())
    selected_name = settings.selected_frameanalysis_name

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
        settings.selected_frameanalysis_name = settings.frameanalysis_items[
            settings.frameanalysis_index
        ].name
    else:
        settings.frameanalysis_index = 0
        settings.selected_frameanalysis_name = ""
    save_config(settings)

    return len(settings.frameanalysis_items)


def get_selected_frameanalysis_path(context: bpy.types.Context) -> str | None:
    settings = context.scene.zaychik_settings
    if not settings.frameanalysis_items:
        return None
    if settings.frameanalysis_index < 0 or settings.frameanalysis_index >= len(
        settings.frameanalysis_items
    ):
        return None
    item = settings.frameanalysis_items[settings.frameanalysis_index]
    settings.selected_frameanalysis_name = item.name
    save_config(settings)
    return item.path


class ZAYCHIK_OT_refresh_frameanalysis_list(Operator):
    bl_idname = "zaychik.refresh_frameanalysis_list"
    bl_label = "Refresh FrameAnalysis List"
    bl_description = "Scan the selected Win64 directory and list all FrameAnalysis folders"

    def execute(self, context: bpy.types.Context) -> set[str]:
        settings = context.scene.zaychik_settings
        root_dir = normalize_path(bpy.path.abspath(settings.dump_root_directory).strip())
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


CLASSES = (
    ZAYCHIK_OT_refresh_frameanalysis_list,
    ZAYCHIK_OT_import_dx12_dump,
)

