from __future__ import annotations

import os
from typing import List, Tuple


def normalize_path(path: str) -> str:
    return os.path.normpath(path)


def resolve_binding_path(root_dir: str, relative_path: str) -> str:
    fixed_relative = relative_path.replace("\\", os.sep)
    return normalize_path(os.path.join(root_dir, fixed_relative))


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

