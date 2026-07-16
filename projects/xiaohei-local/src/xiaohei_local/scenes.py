"""Reusable scene builders — one cognitive anchor per scene."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from .canvas import Canvas
from .character import draw_character

SceneFn = Callable[[Canvas, dict[str, Any]], None]


def scene_book_corrigendum(cv: Canvas, p: dict[str, Any]) -> None:
    st = cv.style
    title = p.get("title", "书固定 · 勘误表换版")
    cv.label((640, 40), title, size=st.small_size, color=st.blue, bg=st.soft_blue, border=st.blue, role="annot", count_label=False)

    cv.rect(100, 155, 400, 515, fill=st.soft, seed=1, radius=8)
    cv.label((250, 130), "书 = DAG 模板", size=st.label_size, role="title", bg=st.chip)
    for i, y in enumerate(range(205, 470, 40)):
        cv.line((130, y), (370, y), width=1.3, color=st.gray, seed=10 + i)
    cv.text((250, 340), p.get("book_body", "Partial → hash → Inter → Sink"), size=st.small_size, color=st.gray, role="annot")
    cv.label((250, 545), "固定", size=st.label_size, role="title")

    draw_character(cv, 540, 325, scale=1.5, pose="idle")
    cv.arrow((475, 320), (405, 295), seed=40)
    cv.arrow((605, 320), (720, 275), seed=41)

    cv.rect(690, 145, 1170, 495, seed=2, radius=8)
    cv.label((930, 125), "勘误表 = 清单", size=st.label_size, role="title", bg=st.chip)
    cv.rect(730, 200, 920, 360, fill=st.soft_red, outline=st.red, seed=3, radius=6)
    cv.label((825, 255), p.get("old_edition", "旧版 e16"), size=st.small_size, color=st.red, bg=st.soft_red, border=st.red, role="annot", tilt=True)
    cv.label((825, 310), "节点坏了", size=st.label_size, color=st.red, bg=st.soft_red, border=st.red, role="title")
    cv.rect(955, 220, 1145, 410, fill=st.soft_orange, seed=4, radius=6)
    cv.label((1050, 275), p.get("new_edition", "新版 e17"), size=st.small_size, role="annot", bg=st.soft_orange, border=st.orange)
    cv.label((1050, 325), "可换版", size=st.label_size, color=st.orange, bg=st.soft_orange, border=st.orange, role="title")
    cv.text((1050, 375), "bucket → node / path", size=st.small_size, color=st.gray, role="annot")

    cv.rect(420, 540, 860, 665, fill=st.soft_orange, seed=5, radius=10)
    cv.label((640, 580), "作业 / 查询", size=st.label_size, role="title", bg=st.soft_orange, border=st.orange)
    cv.label((640, 630), "只绑 ref · 不重印整本书", size=st.small_size, color=st.orange, bg=st.chip, border=st.orange, role="annot")
    cv.arrow((250, 515), (450, 575), width=2.6, seed=50)
    cv.arrow((1050, 495), (820, 575), width=2.6, seed=51)


def scene_lego_first(cv: Canvas, p: dict[str, Any]) -> None:
    st = cv.style
    cv.label((640, 42), p.get("title", "先砖后塔"), size=st.title_size, role="title", bg=st.chip, count_label=False)

    bricks = p.get(
        "bricks",
        [("L1", "接线"), ("L2", "存盘"), ("L3", "时间"), ("L4", "建仓"), ("L5", "清单")],
    )
    base_y = 505
    start_x = 80
    gap = 145
    for i, (lid, name) in enumerate(bricks):
        x = start_x + i * gap
        cv.rect(x, base_y, x + 124, base_y + 100, fill=st.soft, seed=20 + i, radius=8)
        cv.ellipse(x + 30, base_y - 14, x + 52, base_y + 8, fill=st.black, seed=30 + i)
        cv.ellipse(x + 72, base_y - 14, x + 94, base_y + 8, fill=st.black, seed=31 + i)
        cv.label((x + 62, base_y + 35), lid, size=st.label_size, role="title", bg=st.chip)
        cv.text((x + 62, base_y + 72), name, size=st.small_size, color=st.gray, role="annot")

    tower_x = 1000
    stack = list(reversed([b[0] for b in bricks[:-1]]))
    y = 505
    for i, lab in enumerate(stack):
        cv.rect(tower_x, y - 72, tower_x + 155, y, fill=st.soft, seed=40 + i, radius=6)
        cv.label((tower_x + 77, y - 36), lab, size=st.small_size, role="body", bg=st.chip)
        y -= 74
    cv.rect(tower_x, y - 72, tower_x + 155, y, outline=st.orange, width=3.2, seed=50, radius=6)
    cv.label((tower_x + 77, y - 36), f"{bricks[-1][0]}槽", size=st.small_size, color=st.orange, bg=st.soft_orange, border=st.orange, role="annot")
    cv.label((tower_x + 77, 90), p.get("product", "Mode C + KLL"), size=st.label_size, role="title", bg=st.chip)

    last = bricks[-1][0]
    cv.rect(770, 300, 910, 390, fill=st.soft, seed=60, radius=8)
    cv.label((840, 345), last, size=st.label_size, role="title", bg=st.chip)
    draw_character(cv, 840, 248, scale=1.32, pose="carry")
    cv.label((840, 190), "卡进槽位", size=st.small_size, color=st.orange, bg=st.soft_orange, border=st.orange, role="annot", tilt=True)
    cv.arrow((910, 345), (1000, y - 30), seed=61)

    cv.ellipse(140, 135, 360, 305, fill=st.soft, outline=st.red, seed=70)
    cv.label((250, 210), "整坨黏土", size=st.label_size, color=st.red, bg=st.soft_red, border=st.red, role="title")
    cv.x_mark(165, 155, 335, 285, width=4)
    cv.label((250, 335), p.get("forbid", "禁止 · 不默认自研 UDAF"), size=st.small_size, color=st.red, bg=st.soft_red, border=st.red, role="annot")

    cv.label((640, 680), "可独立验收的砖 → 组合成品", size=st.small_size, color=st.blue, bg=st.soft_blue, border=st.blue, role="annot", count_label=False)


def scene_grain_vs_tumble(cv: Canvas, p: dict[str, Any]) -> None:
    st = cv.style
    cv.label((640, 42), p.get("title", "grain ≠ tumble"), size=st.title_size, role="title", bg=st.chip, count_label=False)

    n = int(p.get("minutes", 5))
    x0, x1 = 140, 1140
    y = 435
    cv.line((x0, y), (x1, y), width=3.4, seed=1)
    cell = (x1 - x0) / n
    for i in range(n + 1):
        x = x0 + i * cell
        cv.line((x, y - 14), (x, y + 14), width=2.1, seed=2 + i)
    for i in range(n):
        x = x0 + (i + 0.5) * cell
        cv.ellipse(x - 12, y - 12, x + 12, y + 12, fill=st.black, seed=10 + i)
        cv.text((x, y + 44), f"m{i + 1}", size=st.small_size, color=st.gray, role="annot")

    cv.label(
        (640, y + 95),
        p.get("grain_label", "1min 主键  bucket_ts = ts − ts%60"),
        size=st.label_size,
        role="body",
        bg=st.chip,
    )

    yb = 270
    cv.polyline([(x0, yb + 38), (x0, yb), (x1, yb), (x1, yb + 38)], color=st.orange, width=3.4, seed=20)
    cv.label(
        (640, yb - 28),
        p.get("tumble_label", "tumble = 5min  触发 / 水位（不是主键）"),
        size=st.label_size,
        color=st.orange,
        bg=st.soft_orange,
        border=st.orange,
        role="body",
    )
    cv.label(
        (640, yb + 58),
        p.get("hint", f"一窗最多 {n} 个 1min 行 / service"),
        size=st.small_size,
        color=st.orange,
        bg=st.chip,
        border=st.orange,
        role="annot",
    )

    draw_character(cv, 640, 155, scale=1.35, pose="place")
    cv.label((470, 140), "点主键", size=st.small_size, role="annot", bg=st.chip)
    cv.label((820, 140), "敲触发", size=st.small_size, color=st.orange, bg=st.soft_orange, border=st.orange, role="annot")
    cv.ellipse(860, 115, 925, 175, outline=st.orange, seed=30)
    cv.line((892, 175), (892, 200), color=st.orange, width=2.1, seed=31)

    cv.rect(980, 520, 1210, 665, outline=st.red, seed=40, radius=8)
    cv.label((1095, 565), "日桶当唯一", size=st.small_size, color=st.red, bg=st.soft_red, border=st.red, role="annot")
    cv.label((1095, 615), "禁止当同一尺", size=st.small_size, color=st.red, bg=st.soft_red, border=st.red, role="annot")
    cv.x_mark(1005, 540, 1185, 645, width=3.5)
    cv.label((200, 625), "tumble ≠ grain", size=st.small_size, color=st.blue, bg=st.soft_blue, border=st.blue, role="annot", count_label=False)


def scene_planned_not_shipped(cv: Canvas, p: dict[str, Any]) -> None:
    st = cv.style
    cv.label((640, 38), p.get("title", "规划中 ≠ 已交付"), size=st.title_size, role="title", bg=st.chip, count_label=False)

    cv.rect(370, 100, 1010, 535, outline=st.orange, width=3.6, seed=1, radius=12)
    cv.rect(392, 120, 988, 515, outline=st.orange, width=1.6, seed=2, radius=10)
    cv.label((690, 88), "未开售 · 规划中", size=st.label_size, color=st.orange, bg=st.soft_orange, border=st.orange, role="title")

    cv.label((690, 170), p.get("product", "Mode C 样机"), size=st.label_size, role="title", bg=st.chip)
    cv.polyline([(570, 215), (690, 305), (810, 215), (570, 215)], width=2.3, seed=10)
    cv.text((690, 240), "hash(service)", size=st.small_size, color=st.gray, role="annot")
    for i, lab in enumerate(p.get("buckets", ["FDB0", "FDB1", "FDB2"])):
        x = 510 + i * 125
        cv.rect(x, 345, x + 105, 440, seed=20 + i, radius=6)
        cv.label((x + 52, 392), lab, size=st.small_size, role="annot", bg=st.chip)
    cv.label((690, 475), "bucket → 本机库", size=st.small_size, color=st.gray, bg=st.chip, border=st.gray, role="annot")

    cv.label((845, 315), "非现网 API", size=st.label_size, color=st.red, bg=st.soft_red, border=st.red, role="title")

    draw_character(cv, 245, 348, scale=1.55, pose="press", facing=1)
    cv.label((245, 475), "隔玻璃 · 按不到", size=st.small_size, color=st.blue, bg=st.soft_blue, border=st.blue, role="annot")
    cv.label((245, 525), "勿当已交付", size=st.label_size, color=st.red, bg=st.soft_red, border=st.red, role="title")

    cv.rect(1030, 545, 1235, 680, seed=40, radius=8)
    cv.label((1132, 580), "JM hosts", size=st.small_size, role="annot", bg=st.chip)
    cv.text((1132, 620), "仅提交入口", size=st.small_size, color=st.gray, role="annot")
    cv.label((1132, 655), "≠ bucket", size=st.small_size, color=st.red, bg=st.soft_red, border=st.red, role="annot")

    cv.label(
        (690, 685),
        p.get("footer", "目标形态在柜里 · 现网还没有 bucket 节点产品化"),
        size=st.small_size,
        color=st.blue,
        bg=st.soft_blue,
        border=st.blue,
        role="annot",
        count_label=False,
    )


SCENES: dict[str, SceneFn] = {
    "book_corrigendum": scene_book_corrigendum,
    "lego_first": scene_lego_first,
    "grain_vs_tumble": scene_grain_vs_tumble,
    "planned_not_shipped": scene_planned_not_shipped,
}


def get_scene(name: str) -> SceneFn:
    if name not in SCENES:
        known = ", ".join(sorted(SCENES))
        raise KeyError(f"unknown scene {name!r}; known: {known}")
    return SCENES[name]
