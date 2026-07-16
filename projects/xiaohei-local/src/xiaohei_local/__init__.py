"""Local-first Ian 小黑 16:9 illustrations (recallfs/projects/xiaohei-local)."""

from .canvas import Canvas
from .render import list_scenes, render_scene
from .spec import ShotSpec

__version__ = "0.3.0"
__all__ = ["Canvas", "Style", "ShotSpec", "render_scene", "list_scenes"]

from .style import Style  # noqa: E402
