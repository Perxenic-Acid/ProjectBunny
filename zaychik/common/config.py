from __future__ import annotations

import json
import os
from typing import Dict, Optional

import bpy
from bpy.types import PropertyGroup


CONFIG_FILE_NAME = "Config.json"


def config_path() -> str:
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), CONFIG_FILE_NAME)


def load_config() -> Dict[str, str]:
    path = config_path()
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}
    return {str(key): str(value) for key, value in data.items() if isinstance(value, str)}


def save_config(settings: Optional[PropertyGroup]) -> None:
    if settings is None:
        return
    data = {
        "dump_root_directory": getattr(settings, "dump_root_directory", ""),
        "selected_frameanalysis_name": getattr(settings, "selected_frameanalysis_name", ""),
    }
    try:
        with open(config_path(), "w", encoding="utf-8") as handle:
            json.dump(data, handle, indent=2)
            handle.write("\n")
    except OSError:
        pass


def on_config_property_changed(self: PropertyGroup, context: bpy.types.Context) -> None:
    del context
    save_config(self)

