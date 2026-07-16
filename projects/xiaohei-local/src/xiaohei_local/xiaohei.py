"""小黑 IP drawing — must do the core action, not decorate."""

from __future__ import annotations

from .canvas import Canvas
from .style import Style


def draw_xiaohei(
    cv: Canvas,
    cx: float,
    cy: float,
    *,
    scale: float = 1.0,
    facing: int = 1,
    pose: str = "idle",
) -> None:
    """Draw 小黑 centered at (cx, cy) in logical coordinates.

    pose:
      idle — standing
      carry — slightly leaned, as if carrying
      press — arms forward (glass / wall)
      place — reach down-left (placing dots)
    """
    st: Style = cv.style
    s = scale
    # body (slightly irregular black bean)
    bw, bh = 30 * s, 38 * s
    cv.ellipse(
        cx - bw,
        cy - bh,
        cx + bw,
        cy + bh * 0.5,
        fill=st.black,
        outline=st.black,
        width=1.5,
        seed=int(cx + cy * 3),
    )
    # white dot eyes
    r = 4.8 * s
    eye_y = cy - 8 * s
    span = 9 * s
    for ox in (-span, span):
        cv.ellipse(
            cx + ox - r,
            eye_y - r,
            cx + ox + r,
            eye_y + r,
            fill=st.white,
            outline=st.white,
            width=1,
            seed=11,
        )
    # thin legs
    foot = cy + bh * 0.48
    cv.line((cx - 9 * s, foot), (cx - 11 * s, foot + 24 * s), width=2.2, seed=20)
    cv.line((cx + 9 * s, foot), (cx + 11 * s, foot + 24 * s), width=2.2, seed=21)
    # arms by pose
    ay = cy + 4 * s
    f = 1 if facing >= 0 else -1
    if pose == "press":
        cv.line((cx + 18 * s * f, ay), (cx + 42 * s * f, ay - 2 * s), width=2.2, seed=30)
        cv.line((cx + 18 * s * f, ay + 10 * s), (cx + 42 * s * f, ay + 12 * s), width=2.2, seed=31)
    elif pose == "carry":
        cv.line((cx - 20 * s, ay), (cx - 8 * s, ay + 22 * s), width=2.2, seed=32)
        cv.line((cx + 20 * s, ay), (cx + 8 * s, ay + 22 * s), width=2.2, seed=33)
    elif pose == "place":
        cv.line((cx - 18 * s, ay), (cx - 36 * s, ay + 28 * s), width=2.2, seed=34)
        cv.line((cx + 16 * s, ay), (cx + 28 * s, ay - 8 * s), width=2.2, seed=35)
    else:
        cv.line((cx - 22 * s * f, ay), (cx - 34 * s * f, ay + 10 * s), width=2.0, seed=36)
        cv.line((cx + 22 * s * f, ay), (cx + 34 * s * f, ay + 8 * s), width=2.0, seed=37)
