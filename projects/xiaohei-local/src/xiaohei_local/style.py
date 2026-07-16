"""Style DNA tokens — sync with ian-xiaohei references/style-dna.md."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Style:
    width: int = 1280
    height: int = 720
    ss: int = 3

    white: tuple[int, int, int] = (255, 255, 255)
    black: tuple[int, int, int] = (26, 26, 26)
    red: tuple[int, int, int] = (190, 45, 40)
    orange: tuple[int, int, int] = (228, 108, 26)
    blue: tuple[int, int, int] = (40, 96, 184)
    gray: tuple[int, int, int] = (96, 96, 96)
    soft: tuple[int, int, int] = (251, 251, 251)
    soft_red: tuple[int, int, int] = (255, 247, 245)
    soft_orange: tuple[int, int, int] = (255, 250, 244)
    soft_blue: tuple[int, int, int] = (245, 249, 255)
    chip: tuple[int, int, int] = (255, 255, 255)
    chip_border: tuple[int, int, int] = (48, 48, 48)

    stroke: float = 2.8
    stroke_thin: float = 1.8
    stroke_thick: float = 3.6
    jitter: float = 0.55

    title_size: int = 30
    label_size: int = 24
    small_size: int = 18
    max_labels: int = 8

    margin: int = 52
    subject_min: float = 0.40
    subject_max: float = 0.60

    tracking: float = 0.85
    chip_pad_x: float = 14
    chip_pad_y: float = 8
    chip_radius: float = 11
    # micro hand-note tilt for secondary chips (degrees)
    annot_tilt: float = 1.2


DEFAULT_STYLE = Style()
