"""Optional mascots (not default). Prefer 小黑 via character.draw_character."""

from __future__ import annotations

from .canvas import Canvas
from .style import Style


def draw_snoopy(cv: Canvas, cx: float, cy: float, *, scale=1.0, facing=1, pose="idle") -> None:
    st, s, f = cv.style, max(0.65, scale), (1 if facing >= 0 else -1)
    se, ink, body = cv.smooth_ellipse, st.black, (252, 252, 252)
    gy = cy + 36 * s
    se(cx - 26 * s, gy - 3 * s, cx + 26 * s, gy + 5 * s, fill=(235, 235, 235), outline=(235, 235, 235), width=1)
    body_cy = cy + 10 * s
    se(cx - 28 * s, body_cy - 22 * s, cx + 28 * s, body_cy + 20 * s, fill=body, outline=ink, width=2.0)
    for dx in (-14 * s, 14 * s):
        se(cx + dx - 8 * s, body_cy + 14 * s, cx + dx + 8 * s, body_cy + 28 * s, fill=body, outline=ink, width=1.8)
    hx, hy = cx + 2 * s * f, cy - 26 * s
    se(hx - 34 * s, hy - 8 * s, hx - 10 * s, hy + 40 * s, fill=(28, 28, 28), outline=ink, width=1.7)
    se(hx + 10 * s, hy - 8 * s, hx + 34 * s, hy + 40 * s, fill=(28, 28, 28), outline=ink, width=1.7)
    se(hx - 30 * s, hy - 28 * s, hx + 30 * s, hy + 26 * s, fill=body, outline=ink, width=2.0)
    snx = hx + 10 * s * f
    se(snx - 12 * s, hy + 2 * s, snx + 16 * s, hy + 22 * s, fill=body, outline=ink, width=1.8)
    se(snx + (6 * s if f > 0 else -14 * s), hy + 6 * s, snx + (14 * s if f > 0 else -6 * s), hy + 14 * s, fill=(20, 20, 20), outline=ink, width=1.2)
    ex = hx - 4 * s * f
    se(ex - 4.5 * s, hy - 6 * s, ex + 4.5 * s, hy + 4 * s, fill=ink, outline=ink, width=1)


def draw_xiaohuang_legacy(cv: Canvas, cx: float, cy: float, *, scale=1.0, facing=1, pose="idle") -> None:
    st, s = cv.style, max(0.6, scale)
    se = cv.smooth_ellipse
    Y, YD = st.dog_yellow, st.dog_deep
    body_cy = cy + 8 * s
    se(cx - 28 * s, body_cy - 24 * s, cx + 28 * s, body_cy + 20 * s, fill=Y, outline=st.black, width=1.6)
    hx, hy, hr = cx, cy - 26 * s, 28 * s
    se(hx - hr, hy - hr * 0.9, hx + hr, hy + hr * 0.85, fill=Y, outline=st.black, width=1.6)
    for ox in (-10 * s, 10 * s):
        se(hx + ox - 5 * s, hy - 6 * s, hx + ox + 5 * s, hy + 5 * s, fill=st.black, outline=st.black, width=1)
