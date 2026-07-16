"""Render entrypoints — default mascot: 小黑."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .canvas import Canvas
from .scenes import SCENES, get_scene
from .spec import ShotSpec
from .style import Style, style_for_character


def list_scenes() -> list[str]:
    return sorted(SCENES.keys())


def list_characters() -> list[str]:
    return ["xiaohei", "snoopy", "xiaohuang"]


def render_scene(
    scene: str,
    outfile: str | Path,
    *,
    params: dict[str, Any] | None = None,
    seed: int = 7,
    ss: int = 3,
    character: str = "xiaohei",
    style: Style | None = None,
) -> Path:
    st = style or style_for_character(character, ss=ss)
    cv = Canvas(style=st, seed=seed, character=character)
    # no fluff ambience for 小黑 — clean
    if (character or "").lower() in {"xiaohuang", "huang", "yellow", "小黄", "小黄狗"}:
        cv.ambience_warm(n=8, seed=seed + 3)
    fn = get_scene(scene)
    fn(cv, params or {})
    return cv.save(outfile)


def render_spec(spec: ShotSpec) -> Path:
    return render_scene(
        spec.scene,
        spec.outfile,
        params=spec.merged_params(),
        seed=spec.seed,
        ss=spec.ss,
        character=spec.character,
    )
