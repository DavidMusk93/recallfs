"""Mascot characters: 小黄狗 (default, cute) + 小黑 (classic)."""

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
    """Soft ground oval under mascot."""
    st = cv.style
    y = cy + 38 * s
    cv.ellipse(
        cx - 28 * s,
        y - 6 * s,
        cx + 28 * s,
        y + 8 * s,
        fill=(230, 220, 200),
        outline=(230, 220, 200),
        width=1,
        seed=3,
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
    """Cutier yellow puppy: chibi proportions, collar, sparkle eyes, ground shadow."""
    st: Style = cv.style
    s = max(0.55, scale)
    f = 1 if facing >= 0 else -1

    Y = st.dog_yellow
    YD = st.dog_deep
    EAR = st.dog_ear
    MUZ = st.dog_muzzle
    CHK = st.dog_cheek
    COL = st.dog_collar
    TAG = st.dog_collar_tag

    _shadow(cv, cx, cy, s)

    # --- body (chibi: big head later, compact torso) ---
    bw, bh = 32 * s, 30 * s
    body_cy = cy + 6 * s
    cv.ellipse(
        cx - bw,
        body_cy - bh,
        cx + bw,
        body_cy + bh * 0.7,
        fill=Y,
        outline=st.black,
        width=1.7,
        seed=int(cx + cy) % 97,
    )
    # belly
    cv.ellipse(
        cx - 13 * s,
        body_cy - 4 * s,
        cx + 13 * s,
        body_cy + 20 * s,
        fill=MUZ,
        outline=st.black,
        width=1.15,
        seed=11,
    )

    # --- legs (short chibi stubs) ---
    foot_y = body_cy + bh * 0.55
    for dx in (-11 * s, 11 * s):
        cv.ellipse(
            cx + dx - 8 * s,
            foot_y - 2 * s,
            cx + dx + 8 * s,
            foot_y + 14 * s,
            fill=YD,
            outline=st.black,
            width=1.35,
            seed=20,
        )

    # --- head (larger than body = cuter) ---
    hx, hy = cx, cy - 26 * s
    hr = 30 * s
    # ears behind head
    cv.ellipse(
        hx - 34 * s,
        hy - 4 * s,
        hx - 6 * s,
        hy + 34 * s,
        fill=EAR,
        outline=st.black,
        width=1.45,
        seed=30,
    )
    cv.ellipse(
        hx + 6 * s,
        hy - 4 * s,
        hx + 34 * s,
        hy + 34 * s,
        fill=EAR,
        outline=st.black,
        width=1.45,
        seed=31,
    )
    # inner ear
    cv.ellipse(
        hx - 28 * s,
        hy + 4 * s,
        hx - 12 * s,
        hy + 24 * s,
        fill=CHK,
        outline=CHK,
        width=1,
        seed=32,
    )
    cv.ellipse(
        hx + 12 * s,
        hy + 4 * s,
        hx + 28 * s,
        hy + 24 * s,
        fill=CHK,
        outline=CHK,
        width=1,
        seed=33,
    )

    cv.ellipse(
        hx - hr,
        hy - hr * 0.92,
        hx + hr,
        hy + hr * 0.88,
        fill=Y,
        outline=st.black,
        width=1.75,
        seed=40,
    )

    # collar + tag (between head and body)
    cv.ellipse(
        cx - 18 * s,
        hy + hr * 0.55,
        cx + 18 * s,
        hy + hr * 0.55 + 10 * s,
        fill=COL,
        outline=st.black,
        width=1.3,
        seed=45,
    )
    # bone-ish tag
    tag_x = cx + 14 * s * f
    tag_y = hy + hr * 0.7 + 6 * s
    cv.ellipse(
        tag_x - 6 * s,
        tag_y - 5 * s,
        tag_x + 6 * s,
        tag_y + 5 * s,
        fill=TAG,
        outline=st.black,
        width=1.2,
        seed=46,
    )

    # eyes — bigger sparkles
    eye_y = hy - 2 * s
    for ox in (-11 * s, 11 * s):
        cv.ellipse(
            hx + ox - 6.5 * s,
            eye_y - 7.5 * s,
            hx + ox + 6.5 * s,
            eye_y + 7.5 * s,
            fill=st.black,
            outline=st.black,
            width=1,
            seed=50,
        )
        # dual highlight
        cv.ellipse(
            hx + ox - 3.2 * s,
            eye_y - 5.5 * s,
            hx + ox + 0.5 * s,
            eye_y - 2.0 * s,
            fill=st.white,
            outline=st.white,
            width=1,
            seed=51,
        )
        cv.ellipse(
            hx + ox + 1.2 * s,
            eye_y + 0.5 * s,
            hx + ox + 3.0 * s,
            eye_y + 2.5 * s,
            fill=st.white,
            outline=st.white,
            width=1,
            seed=52,
        )

    # brows (tiny happy curves)
    cv.polyline(
        [
            (hx - 16 * s, hy - 14 * s),
            (hx - 10 * s, hy - 16 * s),
            (hx - 5 * s, hy - 14 * s),
        ],
        color=st.black,
        width=1.35,
        seed=53,
    )
    cv.polyline(
        [
            (hx + 5 * s, hy - 14 * s),
            (hx + 10 * s, hy - 16 * s),
            (hx + 16 * s, hy - 14 * s),
        ],
        color=st.black,
        width=1.35,
        seed=54,
    )

    # cheeks
    for ox in (-20 * s, 12 * s):
        cv.ellipse(
            hx + ox,
            hy + 8 * s,
            hx + ox + 10 * s,
            hy + 16 * s,
            fill=CHK,
            outline=CHK,
            width=1,
            seed=55,
        )

    # muzzle
    cv.ellipse(
        hx - 13 * s,
        hy + 6 * s,
        hx + 13 * s,
        hy + 24 * s,
        fill=MUZ,
        outline=st.black,
        width=1.25,
        seed=56,
    )
    # nose
    cv.ellipse(
        hx - 4.5 * s,
        hy + 8 * s,
        hx + 4.5 * s,
        hy + 14.5 * s,
        fill=st.black,
        outline=st.black,
        width=1,
        seed=57,
    )
    # nose highlight
    cv.ellipse(
        hx - 2.5 * s,
        hy + 8.5 * s,
        hx - 0.2 * s,
        hy + 10.5 * s,
        fill=st.white,
        outline=st.white,
        width=1,
        seed=58,
    )
    # smile
    cv.polyline(
        [
            (hx - 8 * s, hy + 16 * s),
            (hx, hy + 20 * s),
            (hx + 8 * s, hy + 16 * s),
        ],
        color=st.black,
        width=1.7,
        seed=59,
    )
    # tongue tiny
    if pose in {"idle", "wave", "carry"}:
        cv.ellipse(
            hx - 4 * s,
            hy + 18 * s,
            hx + 4 * s,
            hy + 25 * s,
            fill=(255, 130, 140),
            outline=st.black,
            width=1.1,
            seed=60,
        )

    # tail curl
    tx0 = cx - 22 * s * f
    cv.polyline(
        [
            (cx - 18 * s * f, body_cy + 2 * s),
            (tx0 - 8 * s * f, body_cy - 10 * s),
            (tx0 - 4 * s * f, body_cy - 26 * s),
            (tx0 + 10 * s * f, body_cy - 30 * s),
            (tx0 + 14 * s * f, body_cy - 22 * s),
        ],
        color=YD,
        width=4.0 * max(1.0, s),
        seed=70,
    )
    cv.polyline(
        [
            (cx - 18 * s * f, body_cy + 2 * s),
            (tx0 - 8 * s * f, body_cy - 10 * s),
            (tx0 - 4 * s * f, body_cy - 26 * s),
            (tx0 + 10 * s * f, body_cy - 30 * s),
            (tx0 + 14 * s * f, body_cy - 22 * s),
        ],
        color=st.black,
        width=1.35,
        seed=71,
    )

    # pose paws
    ay = body_cy + 4 * s
    if pose == "press":
        for dy in (-4 * s, 12 * s):
            cv.ellipse(
                cx + 20 * s * f,
                ay + dy,
                cx + 42 * s * f,
                ay + dy + 14 * s,
                fill=YD,
                outline=st.black,
                width=1.35,
                seed=80,
            )
    elif pose == "carry":
        for dx in (-20 * s, 6 * s):
            cv.ellipse(
                cx + dx,
                ay + 12 * s,
                cx + dx + 16 * s,
                ay + 28 * s,
                fill=YD,
                outline=st.black,
                width=1.35,
                seed=81,
            )
    elif pose == "place":
        cv.ellipse(
            cx - 38 * s,
            ay + 16 * s,
            cx - 16 * s,
            ay + 32 * s,
            fill=YD,
            outline=st.black,
            width=1.35,
            seed=82,
        )
        cv.ellipse(
            cx + 14 * s,
            ay - 12 * s,
            cx + 34 * s,
            ay + 4 * s,
            fill=YD,
            outline=st.black,
            width=1.35,
            seed=83,
        )
    elif pose == "wave":
        cv.ellipse(
            cx - 28 * s * f,
            ay + 8 * s,
            cx - 10 * s * f,
            ay + 22 * s,
            fill=YD,
            outline=st.black,
            width=1.3,
            seed=84,
        )
        # raised paw
        cv.ellipse(
            cx + 16 * s * f,
            hy - 8 * s,
            cx + 34 * s * f,
            hy + 10 * s,
            fill=YD,
            outline=st.black,
            width=1.35,
            seed=85,
        )
    else:
        for dx in (-28 * s * f, 12 * s * f):
            cv.ellipse(
                cx + dx,
                ay + 8 * s,
                cx + dx + 16 * s,
                ay + 20 * s,
                fill=YD,
                outline=st.black,
                width=1.25,
                seed=86,
            )
