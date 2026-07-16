"""Mascot characters: 小黄狗 (default) + 小黑 (classic)."""

from __future__ import annotations

from .canvas import Canvas
from .style import Style

# palette for 小黄狗
YELLOW = (255, 205, 72)
YELLOW_DEEP = (240, 170, 40)
EAR = (235, 145, 55)
MUZZLE = (255, 248, 230)
NOSE = (40, 40, 40)
CHEEK = (255, 170, 140)


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


def draw_xiaohuang(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    """Cute yellow puppy — round body, floppy ears, doing the core action."""
    st: Style = cv.style
    s = scale
    f = 1 if facing >= 0 else -1

    # body
    bw, bh = 34 * s, 36 * s
    cv.ellipse(
        cx - bw,
        cy - bh * 0.85,
        cx + bw,
        cy + bh * 0.55,
        fill=YELLOW,
        outline=st.black,
        width=1.8,
        seed=int(cx + cy),
    )
    # belly patch
    cv.ellipse(
        cx - 14 * s,
        cy - 2 * s,
        cx + 14 * s,
        cy + 22 * s,
        fill=MUZZLE,
        outline=st.black,
        width=1.2,
        seed=int(cx + 3),
    )

    # head
    hx, hy = cx, cy - 28 * s
    hr = 26 * s
    cv.ellipse(
        hx - hr,
        hy - hr * 0.95,
        hx + hr,
        hy + hr * 0.85,
        fill=YELLOW,
        outline=st.black,
        width=1.8,
        seed=int(cy + 9),
    )

    # floppy ears
    ear_dx = 22 * s * f
    # left ear
    cv.ellipse(
        hx - 30 * s,
        hy - 8 * s,
        hx - 8 * s,
        hy + 28 * s,
        fill=EAR,
        outline=st.black,
        width=1.5,
        seed=50,
    )
    # right ear
    cv.ellipse(
        hx + 8 * s,
        hy - 8 * s,
        hx + 30 * s,
        hy + 28 * s,
        fill=EAR,
        outline=st.black,
        width=1.5,
        seed=51,
    )

    # eyes (cute black ovals + white sparkle)
    eye_y = hy - 2 * s
    for ox in (-10 * s, 10 * s):
        cv.ellipse(
            hx + ox - 5.5 * s,
            eye_y - 6.5 * s,
            hx + ox + 5.5 * s,
            eye_y + 6.5 * s,
            fill=st.black,
            outline=st.black,
            width=1,
            seed=60,
        )
        cv.ellipse(
            hx + ox - 2.2 * s,
            eye_y - 4.5 * s,
            hx + ox + 0.8 * s,
            eye_y - 1.5 * s,
            fill=st.white,
            outline=st.white,
            width=1,
            seed=61,
        )
    # cheeks
    for ox in (-18 * s, 12 * s):
        cv.ellipse(
            hx + ox,
            hy + 6 * s,
            hx + ox + 8 * s,
            hy + 12 * s,
            fill=CHEEK,
            outline=CHEEK,
            width=1,
            seed=62,
        )

    # muzzle + nose + smile
    cv.ellipse(
        hx - 12 * s,
        hy + 4 * s,
        hx + 12 * s,
        hy + 20 * s,
        fill=MUZZLE,
        outline=st.black,
        width=1.3,
        seed=63,
    )
    cv.ellipse(
        hx - 4 * s,
        hy + 6 * s,
        hx + 4 * s,
        hy + 12 * s,
        fill=NOSE,
        outline=st.black,
        width=1,
        seed=64,
    )
    # smile
    cv.polyline(
        [
            (hx - 7 * s, hy + 14 * s),
            (hx, hy + 17 * s),
            (hx + 7 * s, hy + 14 * s),
        ],
        color=st.black,
        width=1.6,
        seed=65,
    )

    # legs
    foot_y = cy + bh * 0.5
    for dx in (-12 * s, 12 * s):
        cv.ellipse(
            cx + dx - 7 * s,
            foot_y - 4 * s,
            cx + dx + 7 * s,
            foot_y + 16 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=70,
        )

    # tail (cute curl toward facing)
    tx = cx - 28 * s * f
    cv.polyline(
        [
            (cx - 20 * s * f, cy + 4 * s),
            (tx, cy - 6 * s),
            (tx - 6 * s * f, cy - 18 * s),
            (tx + 4 * s * f, cy - 22 * s),
        ],
        color=YELLOW_DEEP,
        width=3.2,
        seed=80,
    )
    cv.polyline(
        [
            (cx - 20 * s * f, cy + 4 * s),
            (tx, cy - 6 * s),
            (tx - 6 * s * f, cy - 18 * s),
            (tx + 4 * s * f, cy - 22 * s),
        ],
        color=st.black,
        width=1.4,
        seed=81,
    )

    # pose limbs (simple paws)
    ay = cy + 2 * s
    if pose == "press":
        cv.ellipse(
            cx + 18 * s * f,
            ay - 6 * s,
            cx + 40 * s * f,
            ay + 10 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=90,
        )
        cv.ellipse(
            cx + 18 * s * f,
            ay + 8 * s,
            cx + 40 * s * f,
            ay + 22 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=91,
        )
    elif pose == "carry":
        cv.ellipse(
            cx - 22 * s,
            ay + 10 * s,
            cx - 4 * s,
            ay + 26 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=92,
        )
        cv.ellipse(
            cx + 4 * s,
            ay + 10 * s,
            cx + 22 * s,
            ay + 26 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=93,
        )
    elif pose == "place":
        cv.ellipse(
            cx - 36 * s,
            ay + 14 * s,
            cx - 16 * s,
            ay + 30 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=94,
        )
        cv.ellipse(
            cx + 14 * s,
            ay - 10 * s,
            cx + 32 * s,
            ay + 6 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.4,
            seed=95,
        )
    else:
        # idle paws
        cv.ellipse(
            cx - 30 * s * f,
            ay + 6 * s,
            cx - 12 * s * f,
            ay + 18 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.3,
            seed=96,
        )
        cv.ellipse(
            cx + 12 * s * f,
            ay + 6 * s,
            cx + 30 * s * f,
            ay + 18 * s,
            fill=YELLOW_DEEP,
            outline=st.black,
            width=1.3,
            seed=97,
        )
