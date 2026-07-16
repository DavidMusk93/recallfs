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
    y = cy + 42 * s
    se = cv.smooth_ellipse
    se(cx - 34 * s, y - 6 * s, cx + 34 * s, y + 10 * s, fill=(238, 228, 208), outline=(238, 228, 208), width=1)
    se(cx - 24 * s, y - 2 * s, cx + 24 * s, y + 7 * s, fill=(222, 210, 188), outline=(222, 210, 188), width=1)


def draw_xiaohuang(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    """Honey-grade chibi yellow puppy: layered fur, hearts, collar, sparkles."""
    st: Style = cv.style
    s = max(0.62, scale)
    f = 1 if facing >= 0 else -1
    se = cv.smooth_ellipse

    Y = st.dog_yellow
    YD = st.dog_deep
    YL = (255, 234, 155)
    YB = (255, 218, 110)  # belly-side warm
    EAR = st.dog_ear
    MUZ = st.dog_muzzle
    CHK = st.dog_cheek
    COL = st.dog_collar
    TAG = st.dog_collar_tag
    TONGUE = (255, 120, 142)

    _shadow(cv, cx, cy, s)

    # local hearts + stars (around head)
    cv.heart(cx - 48 * s, cy - 58 * s, r=6.5 * s, color=(255, 155, 175))
    cv.star(cx + 50 * s, cy - 48 * s, r=4.2 * s, color=TAG)
    cv.star(cx - 40 * s, cy - 36 * s, r=3.2 * s, color=COL)
    cv.heart(cx + 44 * s, cy - 66 * s, r=5.0 * s, color=(255, 170, 185))

    body_cy = cy + 10 * s
    # body outer + inner warm band
    se(cx - 31 * s, body_cy - 27 * s, cx + 31 * s, body_cy + 23 * s, fill=Y, outline=st.black, width=1.7)
    se(cx - 22 * s, body_cy - 14 * s, cx + 22 * s, body_cy + 16 * s, fill=YB, outline=YB, width=1)
    se(cx - 14 * s, body_cy - 2 * s, cx + 14 * s, body_cy + 20 * s, fill=MUZ, outline=st.black, width=1.15)

    foot_y = body_cy + 18 * s
    for dx in (-13 * s, 13 * s):
        se(cx + dx - 9.5 * s, foot_y - 2 * s, cx + dx + 9.5 * s, foot_y + 14 * s, fill=YD, outline=st.black, width=1.3)
        for t in (-4.5 * s, 0, 4.5 * s):
            se(
                cx + dx + t - 1.7 * s, foot_y + 8.5 * s,
                cx + dx + t + 1.7 * s, foot_y + 11.8 * s,
                fill=st.black, outline=st.black, width=1,
            )

    # head
    hx, hy = cx, cy - 30 * s
    hr = 34 * s
    se(hx - 38 * s, hy - 2 * s, hx - 2 * s, hy + 38 * s, fill=EAR, outline=st.black, width=1.45)
    se(hx + 2 * s, hy - 2 * s, hx + 38 * s, hy + 38 * s, fill=EAR, outline=st.black, width=1.45)
    se(hx - 32 * s, hy + 8 * s, hx - 12 * s, hy + 28 * s, fill=CHK, outline=CHK, width=1)
    se(hx + 12 * s, hy + 8 * s, hx + 32 * s, hy + 28 * s, fill=CHK, outline=CHK, width=1)

    se(hx - hr, hy - hr * 0.96, hx + hr, hy + hr * 0.9, fill=Y, outline=st.black, width=1.75)
    se(hx - hr * 0.78, hy - hr * 0.72, hx - hr * 0.02, hy + hr * 0.12, fill=YL, outline=YL, width=1)

    # collar + shine + tag
    se(cx - 21 * s, hy + hr * 0.5, cx + 21 * s, hy + hr * 0.5 + 12 * s, fill=COL, outline=st.black, width=1.25)
    se(cx - 13 * s, hy + hr * 0.52, cx + 5 * s, hy + hr * 0.52 + 4.5 * s, fill=(170, 210, 255), outline=(170, 210, 255), width=1)
    tag_x, tag_y = cx + 17 * s * f, hy + hr * 0.7 + 6 * s
    se(tag_x - 7.5 * s, tag_y - 6.5 * s, tag_x + 7.5 * s, tag_y + 6.5 * s, fill=TAG, outline=st.black, width=1.15)
    cv.line((tag_x - 3.5 * s, tag_y), (tag_x + 3.5 * s, tag_y), color=st.black, width=1.05, seed=1)
    se(tag_x - 2 * s, tag_y - 3.5 * s, tag_x + 1 * s, tag_y - 1 * s, fill=st.white, outline=st.white, width=1)

    # eyes (bigger, wetter)
    eye_y = hy - 1.5 * s
    for ox in (-12.5 * s, 12.5 * s):
        se(hx + ox - 7.8 * s, eye_y - 9 * s, hx + ox + 7.8 * s, eye_y + 9 * s, fill=st.black, outline=st.black, width=1)
        se(hx + ox - 4.2 * s, eye_y - 6.5 * s, hx + ox + 1.4 * s, eye_y - 1.6 * s, fill=st.white, outline=st.white, width=1)
        se(hx + ox + 1.6 * s, eye_y + 1.2 * s, hx + ox + 4.2 * s, eye_y + 4.0 * s, fill=st.white, outline=st.white, width=1)
        # tiny lower catchlight
        se(hx + ox - 2 * s, eye_y + 4.5 * s, hx + ox - 0.2 * s, eye_y + 6.2 * s, fill=(200, 220, 255), outline=(200, 220, 255), width=1)

    # brows
    cv.polyline([(hx - 19 * s, hy - 16 * s), (hx - 12 * s, hy - 19 * s), (hx - 5 * s, hy - 16 * s)], color=st.black, width=1.45, seed=2)
    cv.polyline([(hx + 5 * s, hy - 16 * s), (hx + 12 * s, hy - 19 * s), (hx + 19 * s, hy - 16 * s)], color=st.black, width=1.45, seed=3)

    # blush ellipses denser
    se(hx - 26 * s, hy + 9 * s, hx - 12 * s, hy + 20 * s, fill=CHK, outline=CHK, width=1)
    se(hx + 12 * s, hy + 9 * s, hx + 26 * s, hy + 20 * s, fill=CHK, outline=CHK, width=1)

    # muzzle + smile + tongue
    se(hx - 15 * s, hy + 8 * s, hx + 15 * s, hy + 28 * s, fill=MUZ, outline=st.black, width=1.2)
    se(hx - 5.5 * s, hy + 10 * s, hx + 5.5 * s, hy + 17.5 * s, fill=st.black, outline=st.black, width=1)
    se(hx - 3.2 * s, hy + 10.5 * s, hx - 0.3 * s, hy + 13.2 * s, fill=st.white, outline=st.white, width=1)
    cv.polyline([(hx - 10 * s, hy + 19 * s), (hx, hy + 24 * s), (hx + 10 * s, hy + 19 * s)], color=st.black, width=1.8, seed=4)
    if pose in {"idle", "wave", "carry", "place", "love"}:
        se(hx - 5.5 * s, hy + 21 * s, hx + 5.5 * s, hy + 30 * s, fill=TONGUE, outline=st.black, width=1.1)
        cv.line((hx, hy + 22 * s), (hx, hy + 28 * s), color=(220, 85, 105), width=1.05, seed=5)

    # tail
    path = [
        (cx - 20 * s * f, body_cy + 0 * s),
        (cx - 36 * s * f, body_cy - 14 * s),
        (cx - 32 * s * f, body_cy - 30 * s),
        (cx - 16 * s * f, body_cy - 36 * s),
        (cx - 8 * s * f, body_cy - 28 * s),
    ]
    cv.polyline(path, color=YD, width=4.5 * s, seed=6)
    cv.polyline(path, color=st.black, width=1.35, seed=7)
    tip = path[-1]
    se(tip[0] - 6 * s, tip[1] - 6 * s, tip[0] + 6 * s, tip[1] + 6 * s, fill=YD, outline=st.black, width=1.2)

    # paws
    ay = body_cy + 4 * s
    if pose == "press":
        for dy in (-2 * s, 14 * s):
            se(cx + 22 * s * f, ay + dy, cx + 46 * s * f, ay + dy + 14 * s, fill=YD, outline=st.black, width=1.3)
    elif pose == "carry":
        for dx in (-18 * s, 4 * s):
            se(cx + dx, ay + 12 * s, cx + dx + 17 * s, ay + 29 * s, fill=YD, outline=st.black, width=1.3)
    elif pose == "place":
        se(cx - 42 * s, ay + 16 * s, cx - 16 * s, ay + 33 * s, fill=YD, outline=st.black, width=1.3)
        se(cx + 14 * s, ay - 14 * s, cx + 38 * s, ay + 5 * s, fill=YD, outline=st.black, width=1.3)
        # paw prints trail
        for i, (px, py) in enumerate([(cx - 55 * s, ay + 38 * s), (cx - 70 * s, ay + 48 * s)]):
            se(px - 4 * s, py - 3 * s, px + 4 * s, py + 3 * s, fill=(210, 190, 160), outline=(210, 190, 160), width=1)
    elif pose == "wave":
        se(cx - 30 * s * f, ay + 8 * s, cx - 10 * s * f, ay + 22 * s, fill=YD, outline=st.black, width=1.25)
        se(cx + 18 * s * f, hy - 12 * s, cx + 40 * s * f, hy + 10 * s, fill=YD, outline=st.black, width=1.3)
        cv.polyline(
            [(cx + 42 * s * f, hy - 16 * s), (cx + 52 * s * f, hy - 24 * s), (cx + 56 * s * f, hy - 12 * s)],
            color=st.orange, width=1.5, seed=8,
        )
    elif pose == "love":
        se(cx - 28 * s, ay + 10 * s, cx - 10 * s, ay + 24 * s, fill=YD, outline=st.black, width=1.25)
        se(cx + 10 * s, ay + 10 * s, cx + 28 * s, ay + 24 * s, fill=YD, outline=st.black, width=1.25)
        cv.heart(cx, hy - 48 * s, r=9 * s, color=(255, 130, 155))
        cv.heart(cx - 22 * s, hy - 40 * s, r=5.5 * s, color=(255, 160, 180))
        cv.heart(cx + 24 * s, hy - 42 * s, r=5.5 * s, color=(255, 160, 180))
    else:
        for dx in (-30 * s * f, 12 * s * f):
            se(cx + dx, ay + 8 * s, cx + dx + 16 * s, ay + 20 * s, fill=YD, outline=st.black, width=1.2)
