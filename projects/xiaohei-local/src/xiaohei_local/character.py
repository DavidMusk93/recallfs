"""Mascot characters.

Default: **comic beagle** (Snoopy-inspired Peanuts strip feel) — white body,
black ears, minimal black line, almost no fluff.

Also: classic 小黑; legacy 小黄狗 (xiaohuang) kept optional.
"""

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
    name = (character or getattr(cv, "character", None) or "snoopy").lower()
    if name in {"xiaohei", "hei", "black", "小黑"}:
        from .xiaohei import draw_xiaohei

        draw_xiaohei(cv, cx, cy, scale=scale, facing=facing, pose=pose)
        return
    if name in {"xiaohuang", "huang", "yellow", "小黄", "小黄狗"}:
        draw_xiaohuang_legacy(cv, cx, cy, scale=scale, facing=facing, pose=pose)
        return
    # default + aliases: snoopy, comic, beagle, peanuts, 史努比
    draw_snoopy(cv, cx, cy, scale=scale, facing=facing, pose=pose)


def _ground(cv: Canvas, cx: float, cy: float, s: float) -> None:
    """Single flat oval — Peanuts-simple, not soft gradient mush."""
    y = cy + 36 * s
    cv.smooth_ellipse(
        cx - 26 * s,
        y - 3 * s,
        cx + 26 * s,
        y + 5 * s,
        fill=(235, 235, 235),
        outline=(235, 235, 235),
        width=1,
    )


def draw_snoopy(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    """Snoopy-inspired comic beagle: white + black ears, thick simple outlines."""
    st: Style = cv.style
    s = max(0.65, scale)
    f = 1 if facing >= 0 else -1
    se = cv.smooth_ellipse
    ink = st.black
    white = (255, 255, 255)
    # slightly off-white body so it sits on pure paper
    body = (252, 252, 252)
    ear = (28, 28, 28)
    nose = (20, 20, 20)
    w = 2.0  # bold comic outline

    _ground(cv, cx, cy, s)

    body_cy = cy + 10 * s
    # --- body: classic peanut / oval ---
    se(
        cx - 28 * s,
        body_cy - 22 * s,
        cx + 28 * s,
        body_cy + 20 * s,
        fill=body,
        outline=ink,
        width=w,
    )

    # legs — simple black-outlined white stubs (comic)
    foot_y = body_cy + 16 * s
    for dx in (-14 * s, 14 * s):
        se(
            cx + dx - 8 * s,
            foot_y - 2 * s,
            cx + dx + 8 * s,
            foot_y + 12 * s,
            fill=body,
            outline=ink,
            width=w * 0.9,
        )

    # --- head: large oval, slightly left-right by facing ---
    hx = cx + 2 * s * f
    hy = cy - 26 * s
    # long black ears (Snoopy signature) — hang down sides of head
    se(
        hx - 34 * s,
        hy - 8 * s,
        hx - 10 * s,
        hy + 40 * s,
        fill=ear,
        outline=ink,
        width=w * 0.85,
    )
    se(
        hx + 10 * s,
        hy - 8 * s,
        hx + 34 * s,
        hy + 40 * s,
        fill=ear,
        outline=ink,
        width=w * 0.85,
    )

    # head oval
    se(
        hx - 30 * s,
        hy - 28 * s,
        hx + 30 * s,
        hy + 26 * s,
        fill=body,
        outline=ink,
        width=w,
    )

    # snout — small forward oval (comic muzzle)
    snx = hx + 10 * s * f
    se(
        snx - 12 * s,
        hy + 2 * s,
        snx + 16 * s,
        hy + 22 * s,
        fill=body,
        outline=ink,
        width=w * 0.9,
    )
    # nose: solid black oval on snout tip
    se(
        snx + (6 * s if f > 0 else -14 * s),
        hy + 6 * s,
        snx + (14 * s if f > 0 else -6 * s),
        hy + 14 * s,
        fill=nose,
        outline=ink,
        width=1.2,
    )

    # eye — single simple black oval (side-facing comic) or two dots when idle face-on-ish
    if abs(f) >= 0:
        # one visible eye (profile-ish Peanuts energy)
        ex = hx - 4 * s * f
        se(
            ex - 4.5 * s,
            hy - 6 * s,
            ex + 4.5 * s,
            hy + 4 * s,
            fill=ink,
            outline=ink,
            width=1,
        )
        # tiny white glint optional — keep minimal for strip look
        se(
            ex - 2 * s,
            hy - 4 * s,
            ex + 0.5 * s,
            hy - 1.5 * s,
            fill=white,
            outline=white,
            width=1,
        )

    # smile — short simple curve under snout
    cv.polyline(
        [
            (snx - 2 * s * f, hy + 16 * s),
            (snx + 4 * s * f, hy + 18 * s),
            (snx + 8 * s * f, hy + 15 * s),
        ],
        color=ink,
        width=1.6,
        seed=2,
    )

    # collar: thin black line only (not blue plastic)
    cv.polyline(
        [
            (cx - 16 * s, hy + 24 * s),
            (cx - 8 * s, hy + 28 * s),
            (cx + 8 * s, hy + 28 * s),
            (cx + 16 * s, hy + 24 * s),
        ],
        color=ink,
        width=2.0,
        seed=3,
    )

    # tail — simple curve up
    cv.polyline(
        [
            (cx - 24 * s * f, body_cy + 2 * s),
            (cx - 36 * s * f, body_cy - 8 * s),
            (cx - 34 * s * f, body_cy - 22 * s),
        ],
        color=ink,
        width=2.4,
        seed=4,
    )

    # pose limbs — still minimal white ovals
    ay = body_cy + 2 * s
    if pose == "press":
        for dy in (0 * s, 14 * s):
            se(
                cx + 20 * s * f,
                ay + dy,
                cx + 42 * s * f,
                ay + dy + 12 * s,
                fill=body,
                outline=ink,
                width=w * 0.9,
            )
    elif pose == "carry":
        for dx in (-16 * s, 2 * s):
            se(
                cx + dx,
                ay + 10 * s,
                cx + dx + 15 * s,
                ay + 26 * s,
                fill=body,
                outline=ink,
                width=w * 0.9,
            )
    elif pose == "place":
        se(
            cx - 38 * s,
            ay + 14 * s,
            cx - 16 * s,
            ay + 28 * s,
            fill=body,
            outline=ink,
            width=w * 0.9,
        )
        se(
            cx + 12 * s,
            ay - 10 * s,
            cx + 32 * s,
            ay + 4 * s,
            fill=body,
            outline=ink,
            width=w * 0.9,
        )
    elif pose in {"wave", "love", "idle"}:
        if pose == "wave":
            se(
                cx + 16 * s * f,
                hy - 8 * s,
                cx + 34 * s * f,
                hy + 10 * s,
                fill=body,
                outline=ink,
                width=w * 0.9,
            )
            # simple motion lines (comic)
            cv.polyline(
                [
                    (cx + 36 * s * f, hy - 12 * s),
                    (cx + 44 * s * f, hy - 18 * s),
                ],
                color=ink,
                width=1.3,
                seed=5,
            )
        else:
            for dx in (-26 * s * f, 10 * s * f):
                se(
                    cx + dx,
                    ay + 8 * s,
                    cx + dx + 14 * s,
                    ay + 18 * s,
                    fill=body,
                    outline=ink,
                    width=w * 0.85,
                )


def draw_xiaohuang_legacy(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    """Legacy yellow dog (kept for --character xiaohuang). Simpler than v0.7 fluff."""
    st: Style = cv.style
    s = max(0.6, scale)
    f = 1 if facing >= 0 else -1
    se = cv.smooth_ellipse
    Y, YD = st.dog_yellow, st.dog_deep
    _ground(cv, cx, cy, s)
    body_cy = cy + 8 * s
    se(cx - 28 * s, body_cy - 24 * s, cx + 28 * s, body_cy + 20 * s, fill=Y, outline=st.black, width=1.6)
    se(cx - 12 * s, body_cy, cx + 12 * s, body_cy + 18 * s, fill=st.dog_muzzle, outline=st.black, width=1.1)
    hx, hy, hr = cx, cy - 26 * s, 28 * s
    se(hx - 32 * s, hy, hx - 6 * s, hy + 32 * s, fill=st.dog_ear, outline=st.black, width=1.3)
    se(hx + 6 * s, hy, hx + 32 * s, hy + 32 * s, fill=st.dog_ear, outline=st.black, width=1.3)
    se(hx - hr, hy - hr * 0.9, hx + hr, hy + hr * 0.85, fill=Y, outline=st.black, width=1.6)
    for ox in (-10 * s, 10 * s):
        se(hx + ox - 5 * s, hy - 6 * s, hx + ox + 5 * s, hy + 5 * s, fill=st.black, outline=st.black, width=1)
        se(hx + ox - 2 * s, hy - 4 * s, hx + ox + 1 * s, hy - 1 * s, fill=(255, 255, 255), outline=(255, 255, 255), width=1)
    se(hx - 10 * s, hy + 6 * s, hx + 10 * s, hy + 20 * s, fill=st.dog_muzzle, outline=st.black, width=1.15)
    se(hx - 4 * s, hy + 8 * s, hx + 4 * s, hy + 14 * s, fill=st.black, outline=st.black, width=1)
    ay = body_cy + 4 * s
    if pose == "carry":
        for dx in (-16 * s, 2 * s):
            se(cx + dx, ay + 10 * s, cx + dx + 14 * s, ay + 24 * s, fill=YD, outline=st.black, width=1.2)
    elif pose == "press":
        se(cx + 18 * s * f, ay, cx + 38 * s * f, ay + 12 * s, fill=YD, outline=st.black, width=1.2)
    else:
        for dx in (-24 * s * f, 10 * s * f):
            se(cx + dx, ay + 6 * s, cx + dx + 12 * s, ay + 16 * s, fill=YD, outline=st.black, width=1.15)
