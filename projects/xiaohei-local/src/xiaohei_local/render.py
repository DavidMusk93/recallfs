"""Render entrypoints."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .canvas import Canvas
from .scenes import SCENES, get_scene
from .spec import ShotSpec
from .style import Style


def list_scenes() -> list[str]:
    return sorted(SCENES.keys())


def list_characters() -> list[str]:
    return ["xiaohuang", "xiaohei"]


def render_scene(
    scene: str,
    outfile: str | Path,
    *,
    params: dict[str, Any] | None = None,
    seed: int = 7,
    ss: int = 3,
    character: str = "xiaohuang",
    style: Style | None = None,
) -> Path:
    st = style or Style(ss=ss)
    if style is None:
        st = Style(ss=ss)
    cv = Canvas(style=st, seed=seed, character=character)
    fn = get_scene(scene)
    fn(cv, params or {})
    return cv.save(outfile)


def render_spec(spec: ShotSpec) -> Path:
    st = Style(ss=spec.ss)
    return render_scene(
        spec.scene,
        spec.outfile,
        params=spec.merged_params(),
        seed=spec.seed,
        ss=spec.ss,
        character=spec.character,
        style=st,
    )
