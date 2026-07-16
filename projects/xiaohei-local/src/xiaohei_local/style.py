"""Style DNA tokens + warm preset for 小黄狗 scenes."""

from __future__ import annotations

from dataclasses import dataclass, replace


@dataclass(frozen=True)
class Style:
    width: int = 1280
    height: int = 720
    ss: int = 3

    white: tuple[int, int, int] = (255, 255, 255)
    # warm off-white paper (still clean, less clinical than pure ink-on-glare white)
    paper: tuple[int, int, int] = (255, 253, 248)
    black: tuple[int, int, int] = (32, 28, 24)
    red: tuple[int, int, int] = (200, 55, 48)
    orange: tuple[int, int, int] = (232, 120, 36)
    blue: tuple[int, int, int] = (48, 110, 190)
    gray: tuple[int, int, int] = (100, 94, 88)
    soft: tuple[int, int, int] = (252, 248, 242)
    soft_red: tuple[int, int, int] = (255, 244, 240)
    soft_orange: tuple[int, int, int] = (255, 246, 232)
    soft_blue: tuple[int, int, int] = (242, 248, 255)
    soft_yellow: tuple[int, int, int] = (255, 249, 230)
    chip: tuple[int, int, int] = (255, 255, 252)
    chip_border: tuple[int, int, int] = (52, 46, 40)

    stroke: float = 2.6
    stroke_thin: float = 1.7
    stroke_thick: float = 3.5
    jitter: float = 0.48

    title_size: int = 30
    label_size: int = 24
    small_size: int = 18
    max_labels: int = 8

    margin: int = 52
    subject_min: float = 0.40
    subject_max: float = 0.60

    tracking: float = 0.9
    chip_pad_x: float = 14
    chip_pad_y: float = 8.5
    chip_radius: float = 12
    annot_tilt: float = 1.15

    # puppy accents
    dog_yellow: tuple[int, int, int] = (255, 208, 78)
    dog_deep: tuple[int, int, int] = (242, 168, 48)
    dog_ear: tuple[int, int, int] = (236, 148, 62)
    dog_muzzle: tuple[int, int, int] = (255, 250, 236)
    dog_cheek: tuple[int, int, int] = (255, 175, 150)
    dog_collar: tuple[int, int, int] = (90, 165, 240)
    dog_collar_tag: tuple[int, int, int] = (255, 220, 90)


DEFAULT_STYLE = Style()


def style_for_character(character: str, *, ss: int = 3) -> Style:
    """Warm paper + softer ink when using 小黄狗."""
    name = (character or "xiaohuang").lower()
    base = Style(ss=ss)
    if name in {"xiaohei", "hei", "black", "小黑"}:
        # classic cooler white
        return replace(base, paper=(255, 255, 255), white=(255, 255, 255))
    # warm preset
    return replace(
        base,
        white=base.paper,
        soft=base.soft_yellow,
        chip=(255, 255, 250),
        orange=(236, 128, 42),
    )
