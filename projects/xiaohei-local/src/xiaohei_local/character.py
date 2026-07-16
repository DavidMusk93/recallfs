"""Mascot dispatch. Default: simple 小黑."""

from __future__ import annotations

from .canvas import Canvas


def draw_character(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    character: str | None = None,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    name = (character or getattr(cv, "character", None) or "xiaohei").lower()
    if name in {"snoopy", "comic", "beagle", "peanuts", "史努比"}:
        from .character_extra import draw_snoopy

        draw_snoopy(cv, cx, cy, scale=scale, facing=facing, pose=pose)
        return
    if name in {"xiaohuang", "huang", "yellow", "小黄", "小黄狗"}:
        from .character_extra import draw_xiaohuang_legacy

        draw_xiaohuang_legacy(cv, cx, cy, scale=scale, facing=facing, pose=pose)
        return
    # default: 小黑
    from .xiaohei import draw_xiaohei

    draw_xiaohei(cv, cx, cy, scale=scale, facing=facing, pose=pose)
