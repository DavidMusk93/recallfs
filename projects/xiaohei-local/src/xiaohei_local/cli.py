"""CLI: uv run xiaohei-local …"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import __version__
from .fonts import find_cjk_font
from .render import list_characters, list_scenes, render_scene, render_spec
from .spec import ShotSpec


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="xiaohei-local",
        description="Local-first 16:9 illustrations — simple 小黑 default",
    )
    parser.add_argument("--version", action="version", version=f"xiaohei-local {__version__}")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list-scenes", help="List registered scene ids")
    p_list.set_defaults(func=cmd_list)

    p_chars = sub.add_parser("list-characters", help="List mascot ids")
    p_chars.set_defaults(func=cmd_chars)

    p_info = sub.add_parser("info", help="Show font / env readiness")
    p_info.set_defaults(func=cmd_info)

    p_render = sub.add_parser("render", help="Render one scene")
    p_render.add_argument("--scene", required=True, help="Scene id")
    p_render.add_argument("-o", "--outfile", required=True, help="Output .jpg/.png")
    p_render.add_argument("--param", action="append", default=[], help="key=value override")
    p_render.add_argument("--seed", type=int, default=7)
    p_render.add_argument("--ss", type=int, default=3, help="Supersample factor (default 3)")
    p_render.add_argument(
        "--character",
        default="xiaohei",
        choices=list_characters(),
        help="Mascot: xiaohei (default) | snoopy | xiaohuang",
    )
    p_render.set_defaults(func=cmd_render)

    p_spec = sub.add_parser("render-spec", help="Render from JSON shot spec (file or list)")
    p_spec.add_argument("spec", type=Path, help="JSON path: object or array of ShotSpec")
    p_spec.set_defaults(func=cmd_render_spec)

    p_batch = sub.add_parser("render-cube-anchors", help="Render design anchors 08–11 into a dir")
    p_batch.add_argument("-o", "--outdir", type=Path, required=True)
    p_batch.add_argument("--ss", type=int, default=3)
    p_batch.add_argument(
        "--character",
        default="xiaohei",
        choices=list_characters(),
        help="Mascot (default xiaohei)",
    )
    p_batch.set_defaults(func=cmd_cube_anchors)

    p_show = sub.add_parser("showcase", help="2×2 contact sheet of all built-in scenes")
    p_show.add_argument("-o", "--outfile", type=Path, required=True)
    p_show.add_argument("--ss", type=int, default=2, help="Per-tile ss (default 2 for speed)")
    p_show.add_argument("--character", default="xiaohei", choices=list_characters())
    p_show.set_defaults(func=cmd_showcase)

    args = parser.parse_args(argv)
    return int(args.func(args) or 0)


def cmd_list(_: argparse.Namespace) -> int:
    for name in list_scenes():
        print(name)
    return 0


def cmd_chars(_: argparse.Namespace) -> int:
    for name in list_characters():
        print(name)
    return 0


def cmd_info(_: argparse.Namespace) -> int:
    from .fonts import font_report

    print(f"version: {__version__}")
    print(f"cjk_font: {find_cjk_font()}")
    for role, path in font_report().items():
        print(f"  font[{role}]: {path}")
    print(f"scenes: {', '.join(list_scenes())}")
    print(f"characters: {', '.join(list_characters())} (default=xiaohei)")
    print("home: recallfs/projects/xiaohei-local")
    return 0


def _parse_params(items: list[str]) -> dict:
    out: dict = {}
    for it in items:
        if "=" not in it:
            raise SystemExit(f"--param must be key=value, got {it!r}")
        k, v = it.split("=", 1)
        out[k] = v
    return out


def cmd_render(args: argparse.Namespace) -> int:
    path = render_scene(
        args.scene,
        args.outfile,
        params=_parse_params(args.param),
        seed=args.seed,
        ss=args.ss,
        character=args.character,
    )
    print(path)
    return 0


def cmd_render_spec(args: argparse.Namespace) -> int:
    loaded = ShotSpec.load(args.spec)
    specs = loaded if isinstance(loaded, list) else [loaded]
    for spec in specs:
        path = render_spec(spec)
        print(path)
    return 0


def cmd_cube_anchors(args: argparse.Namespace) -> int:
    outdir: Path = args.outdir
    outdir.mkdir(parents=True, exist_ok=True)
    ch = args.character
    jobs = [
        ShotSpec(
            scene="book_corrigendum",
            outfile=str(outdir / "08-book-vs-corrigendum.jpg"),
            title="书固定 · 勘误表换版",
            core_idea="DAG 模板固定；资源清单 edition 可换",
            ss=args.ss,
            character=ch,
        ),
        ShotSpec(
            scene="lego_first",
            outfile=str(outdir / "09-lego-bricks-first.jpg"),
            title="先砖后塔",
            core_idea="L1–L5 组合成 Mode C+KLL；禁止整坨自研 UDAF",
            ss=args.ss,
            character=ch,
        ),
        ShotSpec(
            scene="grain_vs_tumble",
            outfile=str(outdir / "10-grain-1m-vs-tumble-5m.jpg"),
            title="grain ≠ tumble",
            core_idea="1min 主键 ≠ 5min tumble 触发",
            ss=args.ss,
            character=ch,
        ),
        ShotSpec(
            scene="planned_not_shipped",
            outfile=str(outdir / "11-planned-not-shipped.jpg"),
            title="规划中 ≠ 已交付",
            core_idea="Mode C bucket/清单是规划目标，非现网 API",
            ss=args.ss,
            character=ch,
        ),
    ]
    spec_path = outdir / "shots-08-11.json"
    spec_path.write_text(
        json.dumps([j.to_dict() for j in jobs], ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    for j in jobs:
        print(render_spec(j))
    print(spec_path)
    return 0


def cmd_showcase(args: argparse.Namespace) -> int:
    """Render four scenes into a 2×2 contact sheet."""
    from PIL import Image

    from .style import Style

    tmp = args.outfile.parent / f".showcase_tiles_{args.character}"
    tmp.mkdir(parents=True, exist_ok=True)
    scenes = ["book_corrigendum", "lego_first", "grain_vs_tumble", "planned_not_shipped"]
    tiles = []
    for i, sc in enumerate(scenes):
        path = tmp / f"{i}-{sc}.jpg"
        render_scene(sc, path, ss=args.ss, character=args.character, seed=7 + i)
        tiles.append(Image.open(path).convert("RGB"))
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w * 2, h * 2), (255, 253, 248))
    for i, im in enumerate(tiles):
        sheet.paste(im, ((i % 2) * w, (i // 2) * h))
    args.outfile.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.outfile, quality=92, optimize=True, subsampling=0)
    print(args.outfile)
    return 0


if __name__ == "__main__":
    sys.exit(main())
