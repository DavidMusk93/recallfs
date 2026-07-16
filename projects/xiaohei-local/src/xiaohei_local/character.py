"""Mascot characters: 小黄狗 (default, ultra-cute) + 小黑 (classic)."""

from __future__ import annotations

from .canvas import Canvas
from .style import Style


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
    name = (character or getattr(cv, "character", None) or "xiaohuang").lower()
    if name in {"xiaohei", "hei", "black", "小黑"}:
        from .xiaohei import draw_xiaohei

        draw_xiaohei(cv, cx, cy, scale=scale, facing=facing, pose=pose)
        return
    draw_xiaohuang(cv, cx, cy, scale=scale, facing=facing, pose=pose)


def _shadow(cv: Canvas, cx: float, cy: float, s: float) -> None:
    y = cy + 40 * s
    # layered soft shadow
    cv.smooth_ellipse(
        cx - 32 * s, y - 5 * s, cx + 32 * s, y + 9 * s,
        fill=(235, 225, 205), outline=(235, 225, 205), width=1,
    )
    cv.smooth_ellipse(
        cx - 22 * s, y - 2 * s, cx + 22 * s, y + 6 * s,
        fill=(220, 208, 185), outline=(220, 208, 185), width=1,
    )


def draw_xiaohuang(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    """Ultra-cute chibi yellow puppy — smooth shapes, sparkles, collar."""
    st: Style = cv.style
    s = max(0.6, scale)
    f = 1 if facing >= 0 else -1
    se = cv.smooth_ellipse  # cute path

    Y = st.dog_yellow
    YD = st.dog_deep
    YL = (255, 230, 140)  # rim light
    EAR = st.dog_ear
    MUZ = st.dog_muzzle
    CHK = st.dog_cheek
    COL = st.dog_collar
    TAG = st.dog_collar_tag
    TONGUE = (255, 125, 145)

    _shadow(cv, cx, cy, s)

    # floating sparkles near head
    cv.star(cx - 42 * s, cy - 52 * s, r=4.5 * s, color=TAG)
    cv.star(cx + 46 * s, cy - 40 * s, r=3.5 * s, color=COL)
    cv.star(cx + 38 * s, cy - 62 * s, r=3.0 * s, color=YD)

    body_cy = cy + 8 * s
    # body
    se(
        cx - 30 * s, body_cy - 26 * s, cx + 30 * s, body_cy + 22 * s,
        fill=Y, outline=st.black, width=1.65,
    )
    # belly
    se(
        cx - 14 * s, body_cy - 2 * s, cx + 14 * s, body_cy + 20 * s,
        fill=MUZ, outline=st.black, width=1.15,
    )
    # feet
    foot_y = body_cy + 18 * s
    for dx in (-12 * s, 12 * s):
        se(
            cx + dx - 9 * s, foot_y - 2 * s, cx + dx + 9 * s, foot_y + 13 * s,
            fill=YD, outline=st.black, width=1.3,
        )
        # toe dots
        for t in (-4 * s, 0, 4 * s):
            se(
                cx + dx + t - 1.6 * s, foot_y + 8 * s,
                cx + dx + t + 1.6 * s, foot_y + 11.2 * s,
                fill=st.black, outline=st.black, width=1,
            )

    # head
    hx, hy = cx, cy - 28 * s
    hr = 32 * s
    # ears (behind)
    se(hx - 36 * s, hy - 2 * s, hx - 4 * s, hy + 36 * s, fill=EAR, outline=st.black, width=1.4)
    se(hx + 4 * s, hy - 2 * s, hx + 36 * s, hy + 36 * s, fill=EAR, outline=st.black, width=1.4)
    se(hx - 30 * s, hy + 6 * s, hx - 12 * s, hy + 26 * s, fill=CHK, outline=CHK, width=1)
    se(hx + 12 * s, hy + 6 * s, hx + 30 * s, hy + 26 * s, fill=CHK, outline=CHK, width=1)

    se(hx - hr, hy - hr * 0.95, hx + hr, hy + hr * 0.9, fill=Y, outline=st.black, width=1.7)
    # head rim light (left crescent-ish smaller ellipse)
    se(hx - hr * 0.75, hy - hr * 0.7, hx - hr * 0.05, hy + hr * 0.15, fill=YL, outline=YL, width=1)

    # collar
    se(
        cx - 20 * s, hy + hr * 0.52, cx + 20 * s, hy + hr * 0.52 + 11 * s,
        fill=COL, outline=st.black, width=1.25,
    )
    # collar highlight
    se(
        cx - 12 * s, hy + hr * 0.54, cx + 4 * s, hy + hr * 0.54 + 4 * s,
        fill=(160, 200, 255), outline=(160, 200, 255), width=1,
    )
    # tag
    tag_x = cx + 16 * s * f
    tag_y = hy + hr * 0.72 + 5 * s
    se(tag_x - 7 * s, tag_y - 6 * s, tag_x + 7 * s, tag_y + 6 * s, fill=TAG, outline=st.black, width=1.15)
    # tiny bone line on tag
    cv.line((tag_x - 3 * s, tag_y), (tag_x + 3 * s, tag_y), color=st.black, width=1.1, seed=1)

    # eyes
    eye_y = hy - 1 * s
    for ox in (-12 * s, 12 * s):
        se(
            hx + ox - 7.2 * s, eye_y - 8.2 * s, hx + ox + 7.2 * s, eye_y + 8.2 * s,
            fill=st.black, outline=st.black, width=1,
        )
        se(
            hx + ox - 3.8 * s, eye_y - 6 * s, hx + ox + 1.2 * s, eye_y - 1.8 * s,
            fill=st.white, outline=st.white, width=1,
        )
        se(
            hx + ox + 1.5 * s, eye_y + 1.0 * s, hx + ox + 3.8 * s, eye_y + 3.5 * s,
            fill=st.white, outline=st.white, width=1,
        )

    # happy brows
    cv.polyline(
        [(hx - 18 * s, hy - 15 * s), (hx - 11 * s, hy - 18 * s), (hx - 5 * s, hy - 15 * s)],
        color=st.black, width=1.4, seed=2,
    )
    cv.polyline(
        [(hx + 5 * s, hy - 15 * s), (hx + 11 * s, hy - 18 * s), (hx + 18 * s, hy - 15 * s)],
        color=st.black, width=1.4, seed=3,
    )

    # cheeks
    se(hx - 24 * s, hy + 8 * s, hx - 12 * s, hy + 18 * s, fill=CHK, outline=CHK, width=1)
    se(hx + 12 * s, hy + 8 * s, hx + 24 * s, hy + 18 * s, fill=CHK, outline=CHK, width=1)

    # muzzle
    se(hx - 14 * s, hy + 7 * s, hx + 14 * s, hy + 26 * s, fill=MUZ, outline=st.black, width=1.2)
    se(hx - 5 * s, hy + 9 * s, hx + 5 * s, hy + 16 * s, fill=st.black, outline=st.black, width=1)
    se(hx - 3 * s, hy + 9.5 * s, hx - 0.5 * s, hy + 12 * s, fill=st.white, outline=st.white, width=1)
    cv.polyline(
        [(hx - 9 * s, hy + 18 * s), (hx, hy + 22 * s), (hx + 9 * s, hy + 18 * s)],
        color=st.black, width=1.75, seed=4,
    )
    # tongue
    if pose in {"idle", "wave", "carry", "place"}:
        se(hx - 5 * s, hy + 20 * s, hx + 5 * s, hy + 28 * s, fill=TONGUE, outline=st.black, width=1.1)
        cv.line((hx, hy + 21 * s), (hx, hy + 26 * s), color=(220, 90, 110), width=1.0, seed=5)

    # tail
    path = [
        (cx - 20 * s * f, body_cy + 0 * s),
        (cx - 34 * s * f, body_cy - 12 * s),
        (cx - 30 * s * f, body_cy - 28 * s),
        (cx - 16 * s * f, body_cy - 34 * s),
        (cx - 10 * s * f, body_cy - 26 * s),
    ]
    cv.polyline(path, color=YD, width=4.2 * s, seed=6)
    cv.polyline(path, color=st.black, width=1.35, seed=7)
    # tail tip fluff
    tip = path[-1]
    se(tip[0] - 5 * s, tip[1] - 5 * s, tip[0] + 5 * s, tip[1] + 5 * s, fill=YD, outline=st.black, width=1.2)

    # paws by pose
    ay = body_cy + 4 * s
    if pose == "press":
        for dy in (-2 * s, 14 * s):
            se(
                cx + 22 * s * f, ay + dy, cx + 44 * s * f, ay + dy + 13 * s,
                fill=YD, outline=st.black, width=1.3,
            )
    elif pose == "carry":
        for dx in (-18 * s, 4 * s):
            se(cx + dx, ay + 12 * s, cx + dx + 16 * s, ay + 28 * s, fill=YD, outline=st.black, width=1.3)
    elif pose == "place":
        se(cx - 40 * s, ay + 16 * s, cx - 16 * s, ay + 32 * s, fill=YD, outline=st.black, width=1.3)
        se(cx + 14 * s, ay - 14 * s, cx + 36 * s, ay + 4 * s, fill=YD, outline=st.black, width=1.3)
    elif pose == "wave":
        se(cx - 30 * s * f, ay + 8 * s, cx - 10 * s * f, ay + 22 * s, fill=YD, outline=st.black, width=1.25)
        se(cx + 18 * s * f, hy - 10 * s, cx + 38 * s * f, hy + 10 * s, fill=YD, outline=st.black, width=1.3)
        # motion arcs
        cv.polyline(
            [
                (cx + 40 * s * f, hy - 14 * s),
                (cx + 48 * s * f, hy - 20 * s),
                (cx + 52 * s * f, hy - 10 * s),
            ],
            color=st.orange, width=1.4, seed=8,
        )
    else:
        for dx in (-30 * s * f, 12 * s * f):
            se(cx + dx, ay + 8 * s, cx + dx + 16 * s, ay + 20 * s, fill=YD, outline=st.black, width=1.2)
