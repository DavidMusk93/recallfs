"""Style DNA — clean paper for 小黑 default."""

from __future__ import annotations

from dataclasses import dataclass, replace


@dataclass(frozen=True)
class Style:
    width: int = 1280
    height: int = 720
    ss: int = 3

    white: tuple[int, int, int] = (255, 255, 255)
    paper: tuple[int, int, int] = (255, 255, 255)
    black: tuple[int, int, int] = (22, 22, 22)
    red: tuple[int, int, int] = (196, 48, 42)
    orange: tuple[int, int, int] = (230, 112, 28)
    blue: tuple[int, int, int] = (42, 98, 186)
    gray: tuple[int, int, int] = (110, 110, 110)
    soft: tuple[int, int, int] = (250, 250, 250)
    soft_red: tuple[int, int, int] = (255, 246, 244)
    soft_orange: tuple[int, int, int] = (255, 248, 240)
    soft_blue: tuple[int, int, int] = (244, 248, 255)
    soft_yellow: tuple[int, int, int] = (255, 252, 240)
    chip: tuple[int, int, int] = (255, 255, 255)
    chip_border: tuple[int, int, int] = (48, 48, 48)

    stroke: float = 2.5
    stroke_thin: float = 1.7
    stroke_thick: float = 3.4
    jitter: float = 0.5

    title_size: int = 30
    label_size: int = 24
    small_size: int = 18
    max_labels: int = 8

    margin: int = 52
    subject_min: float = 0.40
    subject_max: float = 0.60

    tracking: float = 0.85
    chip_pad_x: float = 13
    chip_pad_y: float = 8
    chip_radius: float = 10
    annot_tilt: float = 0.6

    dog_yellow: tuple[int, int, int] = (255, 208, 78)
    dog_deep: tuple[int, int, int] = (242, 168, 48)
    dog_ear: tuple[int, int, int] = (236, 148, 62)
    dog_muzzle: tuple[int, int, int] = (255, 250, 236)
    dog_cheek: tuple[int, int, int] = (255, 175, 150)
    dog_collar: tuple[int, int, int] = (90, 165, 240)
    dog_collar_tag: tuple[int, int, int] = (255, 220, 90)


DEFAULT_STYLE = Style()


def style_for_character(character: str, *, ss: int = 3) -> Style:
    name = (character or "xiaohei").lower()
    base = Style(ss=ss)
    if name in {"xiaohuang", "huang", "yellow", "小黄", "小黄狗"}:
        return replace(base, paper=(255, 253, 248), soft=base.soft_yellow)
    if name in {"snoopy", "comic", "beagle"}:
        return replace(base, orange=(180, 50, 50), annot_tilt=0.0)
    # 小黑 default: pure white, classic DNA colors
    return base
