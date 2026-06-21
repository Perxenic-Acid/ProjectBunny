from __future__ import annotations

import importlib
import sys
import types
from typing import TYPE_CHECKING, ClassVar, Tuple

import bpy
from bpy.props import PointerProperty

# These are imported later after reload, but we declare them here for type hints
if TYPE_CHECKING:
    from .common import properties
    from .common.config import ConfigManager
    from .ui import operators, panel
    from .ui.operators import FrameAnalysisUI


bl_info = {
    "name": "Zaychik DX12 Dump Importer",
    "author": "OpenAI",
    "description": "Analyze a DX12 frame dump log.jsonl and try importing meshes into Blender",
    "blender": (4, 2, 0),
    "version": (0, 1, 0),
    "location": "View3D > Sidebar > Zaychik",
    "warning": "Early prototype importer for DX12 frame dumps",
    "category": "Import-Export",
}


class ZaychikAddon:
    """Hosts all addon lifecycle logic (register/unregister/config restore).

    Only the two module-level ``register``/``unregister`` wrappers at the
    bottom of this file live outside a class — Blender's addon API looks for
    those exact module-level callables and cannot be changed.
    """

    _SUBMODULE_NAMES: ClassVar[Tuple[str, ...]] = (
        "zaychik.common.config",
        "zaychik.common.properties",
        "zaychik.utils.formats",
        "zaychik.utils.layouts",
        "zaychik.utils.paths",
        "zaychik.utils.parser",
        "zaychik.utils.importer",
        "zaychik.ui.operators",
        "zaychik.ui.panel",
    )

    _restore_timer: ClassVar[object] = None  # stored function reference for unregister

    # ------------------------------------------------------------------
    # Submodule hot-reload
    # ------------------------------------------------------------------

    @classmethod
    def _reload_submodules(cls) -> None:
        """Force-reload cached submodules so F8 / script reload picks up edits."""
        for name in cls._SUBMODULE_NAMES:
            module = sys.modules.get(name)
            if isinstance(module, types.ModuleType):
                importlib.reload(module)

    # ------------------------------------------------------------------
    # Delayed init
    # ------------------------------------------------------------------

    @classmethod
    def _restore_config_and_refresh(cls) -> None:
        """One-shot timer: restore saved config and refresh the FrameAnalysis list.

        Runs via bpy.app.timers shortly after register() returns, so all classes
        are registered, the PointerProperty is attached, and we are outside the
        addon-enable context (where writing ID data can raise
        "Writing to ID classes in this context is not allowed").
        """
        try:
            from .common.config import ConfigManager
            from .ui.operators import FrameAnalysisUI

            config = ConfigManager.load_config()
            for scene in bpy.data.scenes:
                settings = scene.zaychik_settings
                settings.dump_root_directory = config.get("dump_root_directory", "")
                settings.selected_frameanalysis_name = config.get(
                    "selected_frameanalysis_name", ""
                )

            context = bpy.context
            if context and context.scene and context.scene.zaychik_settings.dump_root_directory:
                FrameAnalysisUI.refresh_items(context)
        except Exception:
            # Initialization failures must never break addon loading.
            pass
        return None  # don't reschedule

    # ------------------------------------------------------------------
    # Register / unregister
    # ------------------------------------------------------------------

    @classmethod
    def register(cls) -> None:
        # Import after submodule reload so names point at the freshly-loaded modules.
        from .common import properties
        from .ui import operators, panel
        from .common.properties import ZAYCHIK_PG_settings

        if hasattr(bpy.types.Scene, "zaychik_settings"):
            try:
                del bpy.types.Scene.zaychik_settings
            except Exception:
                pass

        # Register each module
        properties.register()
        operators.register()
        panel.register()

        bpy.types.Scene.zaychik_settings = PointerProperty(type=ZAYCHIK_PG_settings)

        # Schedule the first restore/refresh once the addon is fully loaded.
        cls._restore_timer = cls._restore_config_and_refresh
        bpy.app.timers.register(cls._restore_timer, first_interval=0.1)

    @classmethod
    def unregister(cls) -> None:
        # Defensive cleanup: the timer might still be pending.
        if cls._restore_timer is not None:
            try:
                bpy.app.timers.unregister(cls._restore_timer)
            except (ValueError, KeyError, RuntimeError):
                pass
            cls._restore_timer = None

        if hasattr(bpy.types.Scene, "zaychik_settings"):
            del bpy.types.Scene.zaychik_settings

        # Unregister in reverse order
        from .ui import operators, panel
        from .common import properties

        try:
            panel.unregister()
        except Exception:
            pass
        try:
            operators.unregister()
        except Exception:
            pass
        try:
            properties.unregister()
        except Exception:
            pass


# Hot-reload submodules BEFORE importing classes, so the names imported below
# refer to the freshly-loaded versions (avoids stale pyc / old class references
# on F8 Reload Scripts).
ZaychikAddon._reload_submodules()


# ----------------------------------------------------------------------
# Blender addon entry points — Blender looks for these module-level names.
# They are thin forwards into the ZaychikAddon class so all logic lives on
# the class, satisfying the project's "no free functions" rule while
# respecting Blender's addon API contract.
# ----------------------------------------------------------------------

def register() -> None:
    ZaychikAddon.register()


def unregister() -> None:
    ZaychikAddon.unregister()
