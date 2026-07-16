"""小黑 IP — 简简单单：黑豆身体、白点眼、细腿，认真干活。"""

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
    """Minimal 小黑: solid black body, two white eyes, stick legs + pose arms."""
    st: Style = cv.style
    s = max(0.7, scale)
    f = 1 if facing >= 0 else -1
    se = cv.smooth_ellipse  # clean blob, not messy wobble

    # soft ground dash (very light, optional presence)
    gy = cy + 34 * s
    se(
        cx - 16 * s,
        gy - 2 * s,
        cx + 16 * s,
        gy + 3 * s,
        fill=(240, 240, 240),
        outline=(240, 240, 240),
        width=1,
    )

    # body — black bean
    bw, bh = 28 * s, 36 * s
    se(
        cx - bw,
        cy - bh,
        cx + bw,
        cy + bh * 0.45,
        fill=st.black,
        outline=st.black,
        width=1.2,
    )

    # eyes — plain white dots
    r = 5.0 * s
    eye_y = cy - 7 * s
    span = 8.5 * s
    for ox in (-span, span):
        se(
            cx + ox - r,
            eye_y - r,
            cx + ox + r,
            eye_y + r,
            fill=st.white,
            outline=st.white,
            width=1,
        )

    # stick legs
    foot = cy + bh * 0.42
    cv.line((cx - 8 * s, foot), (cx - 9 * s, foot + 22 * s), width=2.3, seed=20)
    cv.line((cx + 8 * s, foot), (cx + 9 * s, foot + 22 * s), width=2.3, seed=21)

    # stick arms by pose
    ay = cy + 3 * s
    if pose == "press":
        cv.line((cx + 16 * s * f, ay), (cx + 40 * s * f, ay - 1 * s), width=2.2, seed=30)
        cv.line((cx + 16 * s * f, ay + 10 * s), (cx + 40 * s * f, ay + 12 * s), width=2.2, seed=31)
    elif pose == "carry":
        cv.line((cx - 18 * s, ay), (cx - 6 * s, ay + 20 * s), width=2.2, seed=32)
        cv.line((cx + 18 * s, ay), (cx + 6 * s, ay + 20 * s), width=2.2, seed=33)
    elif pose == "place":
        cv.line((cx - 16 * s, ay), (cx - 34 * s, ay + 26 * s), width=2.2, seed=34)
        cv.line((cx + 14 * s, ay), (cx + 26 * s, ay - 6 * s), width=2.2, seed=35)
    elif pose == "wave":
        cv.line((cx - 18 * s * f, ay + 6 * s), (cx - 30 * s * f, ay + 14 * s), width=2.0, seed=36)
        cv.line((cx + 16 * s * f, ay), (cx + 28 * s * f, ay - 18 * s), width=2.2, seed=37)
    else:
        # idle / love → calm arms
        cv.line((cx - 20 * s * f, ay), (cx - 30 * s * f, ay + 8 * s), width=2.0, seed=38)
        cv.line((cx + 20 * s * f, ay), (cx + 30 * s * f, ay + 6 * s), width=2.0, seed=39)
