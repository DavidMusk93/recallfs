"""CJK font discovery with role faces + face metrics helpers."""

from __future__ import annotations

import os
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from PIL import ImageFont

# Prefer modern geometric sans; Medium for titles; Light for fine notes.
_TITLE = [
    ("/System/Library/Fonts/Hiragino Sans GB.ttc", 2),  # often W3/W6 variants
    ("/System/Library/Fonts/Hiragino Sans GB.ttc", 0),
    ("/System/Library/Fonts/STHeiti Medium.ttc", 0),
    ("/System/Library/Fonts/STHeiti Light.ttc", 0),
    ("/Library/Fonts/Arial Unicode.ttf", 0),
    ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0),
    ("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0),
]

_BODY = [
    ("/System/Library/Fonts/Hiragino Sans GB.ttc", 0),
    ("/System/Library/Fonts/STHeiti Medium.ttc", 0),
    ("/System/Library/Fonts/STHeiti Light.ttc", 0),
    ("/Library/Fonts/Arial Unicode.ttf", 0),
    ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0),
    ("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0),
]

_ANNOT = [
    ("/System/Library/Fonts/Hiragino Sans GB.ttc", 0),
    ("/System/Library/Fonts/STHeiti Light.ttc", 0),
    ("/System/Library/Fonts/STHeiti Medium.ttc", 0),
    ("/Library/Fonts/Arial Unicode.ttf", 0),
    ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0),
]


@dataclass(frozen=True)
class FontFace:
    path: str
    index: int = 0


def _try_face(path: str, index: int) -> FontFace | None:
    if not Path(path).is_file():
        return None
    try:
        ImageFont.truetype(path, size=24, index=index)
        return FontFace(path, index)
    except OSError:
        if index != 0:
            try:
                ImageFont.truetype(path, size=24, index=0)
                return FontFace(path, 0)
            except OSError:
                return None
        return None


def _pick(cands: list[tuple[str, int]]) -> FontFace:
    env = os.environ.get("XIAOHEI_FONT")
    if env and Path(env).is_file():
        return FontFace(env, 0)
    for path, idx in cands:
        face = _try_face(path, idx)
        if face:
            return face
    raise FileNotFoundError(
        "No CJK font found. Set XIAOHEI_FONT=/path/to/NotoSansSC-Regular.otf"
    )


@lru_cache(maxsize=1)
def face_title() -> FontFace:
    return _pick(_TITLE)


@lru_cache(maxsize=1)
def face_body() -> FontFace:
    return _pick(_BODY)


@lru_cache(maxsize=1)
def face_annot() -> FontFace:
    return _pick(_ANNOT)


@lru_cache(maxsize=96)
def load_font(size: int, *, role: str = "body"):
    face = {"title": face_title, "body": face_body, "annot": face_annot}.get(role, face_body)()
    try:
        return ImageFont.truetype(face.path, size=size, index=face.index)
    except OSError:
        return ImageFont.truetype(face.path, size=size)


def find_cjk_font() -> str:
    return face_body().path


def font_report() -> dict[str, str]:
    return {
        "title": f"{face_title().path}#{face_title().index}",
        "body": f"{face_body().path}#{face_body().index}",
        "annot": f"{face_annot().path}#{face_annot().index}",
    }
