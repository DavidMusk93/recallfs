"""High-DPI canvas: calm ink + polished CJK chips (v0.3)."""

from __future__ import annotations

import math
import random
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

from .fonts import load_font
from .style import DEFAULT_STYLE, Style


@dataclass
class Canvas:
    style: Style = DEFAULT_STYLE
    seed: int = 7
    character: str = "xiaohei"  # default: simple 小黑

    def __post_init__(self) -> None:
        s = self.style
        self._ss = max(1, int(s.ss))
        self._W = s.width * self._ss
        self._H = s.height * self._ss
        bg = getattr(s, "paper", None) or s.white
        self.img = Image.new("RGB", (self._W, self._H), bg)
        self.draw = ImageDraw.Draw(self.img)
        self._label_count = 0
        self.character = (self.character or "xiaohei").lower()

    def sx(self, x: float) -> float:
        return x * self._ss

    def sy(self, y: float) -> float:
        return y * self._ss

    def sn(self, n: float) -> int:
        return max(1, int(round(float(n) * self._ss)))

    def box(self, x0, y0, x1, y1):
        return (self.sx(x0), self.sy(y0), self.sx(x1), self.sy(y1))

    def _jitter_polyline(self, pts, *, fill, width, seed=None):
        rng = random.Random(self.seed if seed is None else seed)
        j = self.style.jitter * self._ss
        if len(pts) < 2:
            return
        path = []
        for i in range(len(pts) - 1):
            x1, y1 = pts[i]
            x2, y2 = pts[i + 1]
            dist = math.hypot(x2 - x1, y2 - y1)
            steps = max(2, int(dist / max(3.5, 4.5 * self._ss)))
            for s in range(steps + 1):
                t = s / steps
                # ease endpoints less jitter
                edge = min(t, 1 - t) * 2
                jj = j * (0.35 + 0.65 * edge)
                path.append(
                    (
                        x1 + (x2 - x1) * t + rng.uniform(-jj, jj),
                        y1 + (y2 - y1) * t + rng.uniform(-jj, jj),
                    )
                )
        self.draw.line(path, fill=fill, width=max(1, width), joint="curve")

    def line(self, p1, p2, *, color=None, width=None, seed=0):
        c = color or self.style.black
        w = self.sn(width if width is not None else self.style.stroke)
        self._jitter_polyline(
            [(self.sx(p1[0]), self.sy(p1[1])), (self.sx(p2[0]), self.sy(p2[1]))],
            fill=c,
            width=w,
            seed=seed,
        )

    def polyline(self, pts, *, color=None, width=None, seed=0):
        c = color or self.style.black
        w = self.sn(width if width is not None else self.style.stroke)
        self._jitter_polyline(
            [(self.sx(x), self.sy(y)) for x, y in pts],
            fill=c,
            width=w,
            seed=seed,
        )

    def rect(
        self,
        x0,
        y0,
        x1,
        y1,
        *,
        fill=None,
        outline=None,
        width=None,
        seed=1,
        radius=0,
    ):
        outline = outline or self.style.black
        w = self.sn(width if width is not None else self.style.stroke)
        if radius > 0:
            box = self.box(x0, y0, x1, y1)
            r = self.sn(radius)
            if fill:
                self.draw.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=w)
            else:
                self.draw.rounded_rectangle(box, radius=r, outline=outline, width=w)
            return
        if fill:
            self.draw.rectangle(self.box(x0, y0, x1, y1), fill=fill)
        pts = [
            (self.sx(x0), self.sy(y0)),
            (self.sx(x1), self.sy(y0)),
            (self.sx(x1), self.sy(y1)),
            (self.sx(x0), self.sy(y1)),
            (self.sx(x0), self.sy(y0)),
        ]
        self._jitter_polyline(pts, fill=outline, width=w, seed=seed)

    def ellipse(self, x0, y0, x1, y1, *, fill=None, outline=None, width=None, seed=2):
        """Slightly wobbly ellipse (diagram ink)."""
        outline = outline or self.style.black
        w = self.sn(width if width is not None else self.style.stroke)
        cx = (self.sx(x0) + self.sx(x1)) / 2
        cy = (self.sy(y0) + self.sy(y1)) / 2
        rx = abs(self.sx(x1) - self.sx(x0)) / 2
        ry = abs(self.sy(y1) - self.sy(y0)) / 2
        rng = random.Random(seed + self.seed)
        pts = []
        for i in range(64):
            a = 2 * math.pi * i / 64
            j = rng.uniform(0.99, 1.01)
            pts.append((cx + rx * math.cos(a) * j, cy + ry * math.sin(a) * j))
        pts.append(pts[0])
        if fill:
            self.draw.polygon(pts[:-1], fill=fill)
        self._jitter_polyline(pts, fill=outline, width=w, seed=seed)

    def smooth_ellipse(
        self,
        x0,
        y0,
        x1,
        y1,
        *,
        fill=None,
        outline=None,
        width=None,
    ):
        """Clean ellipse for mascot (cute, not wobbly)."""
        w = self.sn(width if width is not None else self.style.stroke_thin)
        box = self.box(x0, y0, x1, y1)
        if fill and outline:
            self.draw.ellipse(box, fill=fill, outline=outline, width=w)
        elif fill:
            self.draw.ellipse(box, fill=fill)
        else:
            self.draw.ellipse(box, outline=outline or self.style.black, width=w)

    def star(self, cx, cy, r=6, *, color=None, points=4):
        """Tiny sparkle (for 小黄狗 ambience)."""
        c = color or self.style.orange
        pts = []
        for i in range(points * 2):
            ang = math.pi / 2 + i * math.pi / points
            rad = r if i % 2 == 0 else r * 0.35
            pts.append((cx + rad * math.cos(ang), cy - rad * math.sin(ang)))
        sp = [(self.sx(x), self.sy(y)) for x, y in pts]
        self.draw.polygon(sp, fill=c)

    def heart(self, cx, cy, r=8, *, color=None):
        """Tiny filled heart (cute accent)."""
        c = color or (255, 140, 160)
        # two circles + triangle via polygon
        se = self.smooth_ellipse
        se(cx - r * 0.7, cy - r * 0.55, cx + r * 0.05, cy + r * 0.35, fill=c, outline=c, width=1)
        se(cx - r * 0.05, cy - r * 0.55, cx + r * 0.7, cy + r * 0.35, fill=c, outline=c, width=1)
        pts = [
            (self.sx(cx - r * 0.72), self.sy(cy + r * 0.05)),
            (self.sx(cx + r * 0.72), self.sy(cy + r * 0.05)),
            (self.sx(cx), self.sy(cy + r * 0.95)),
        ]
        self.draw.polygon(pts, fill=c)

    def soft_orb(self, cx, cy, r=20, *, color=None, alpha: int = 40):
        """Soft bokeh blob via temporary RGBA layer."""
        c = color or (255, 220, 160)
        R = self.sn(r)
        layer = Image.new("RGBA", (R * 2 + 4, R * 2 + 4), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        ld.ellipse((2, 2, R * 2, R * 2), fill=c + (alpha,))
        layer = layer.filter(ImageFilter.GaussianBlur(radius=max(1.5, R * 0.22)))
        self.img.paste(
            layer,
            (int(self.sx(cx) - R - 2), int(self.sy(cy) - R - 2)),
            layer,
        )

    def ambience_warm(self, *, n: int = 14, seed: int = 99):
        """Warm bokeh + stars + occasional hearts — cute-dog scenes only."""
        if getattr(self, "character", "") not in {"xiaohuang", "huang", "dog", "小黄", "小黄狗"}:
            return
        rng = random.Random(self.seed + seed)
        st = self.style
        # soft bokeh first (behind everything drawn later if called early)
        for _ in range(max(4, n // 2)):
            x = rng.uniform(st.margin, st.width - st.margin)
            y = rng.uniform(st.margin * 0.5, st.height * 0.55)
            self.soft_orb(
                x,
                y,
                r=rng.uniform(14, 36),
                color=rng.choice([(255, 236, 190), (255, 220, 170), (255, 210, 200)]),
                alpha=rng.randint(22, 42),
            )
        colors = [st.dog_yellow, st.dog_collar_tag, (255, 230, 180), st.dog_collar]
        for _ in range(n):
            x = rng.uniform(st.margin, st.width - st.margin)
            y = rng.uniform(st.margin, st.height * 0.45)
            r = rng.uniform(2.2, 5.2)
            self.star(x, y, r=r, color=rng.choice(colors), points=4)
        # 2–3 floating hearts
        for _ in range(rng.randint(2, 3)):
            self.heart(
                rng.uniform(st.margin + 40, st.width - st.margin - 40),
                rng.uniform(st.margin + 20, st.height * 0.38),
                r=rng.uniform(5, 8),
                color=rng.choice([(255, 150, 170), (255, 170, 185)]),
            )

    def arrow(self, p1, p2, *, color=None, width=2.8, head=13, seed=3):
        c = color or self.style.orange
        self.line(p1, p2, color=c, width=width, seed=seed)
        ang = math.atan2(p2[1] - p1[1], p2[0] - p1[0])
        x2, y2 = self.sx(p2[0]), self.sy(p2[1])
        L = head * self._ss
        a1, a2 = ang + 2.5, ang - 2.5
        self.draw.polygon(
            [
                (x2, y2),
                (x2 - L * math.cos(a1), y2 - L * math.sin(a1)),
                (x2 - L * math.cos(a2), y2 - L * math.sin(a2)),
            ],
            fill=c,
        )

    def _role_for(self, size, role):
        if role:
            return role
        if size >= self.style.title_size - 1:
            return "title"
        if size <= self.style.small_size + 1:
            return "annot"
        return "body"

    def _measure(self, s, font):
        bbox = self.draw.textbbox((0, 0), s, font=font)
        return bbox[2] - bbox[0], bbox[3] - bbox[1]

    def _compose_string_layer(self, s, font, color, track_px):
        widths = []
        total_w = 0
        max_h = 0
        for ch in s:
            w, h = self._measure(ch, font)
            widths.append(w)
            total_w += w + track_px
            max_h = max(max_h, h)
        if s:
            total_w -= track_px
        # vertical metrics: extra headroom for CJK
        ascent_pad = self.sn(2)
        pad = self.sn(4)
        layer = Image.new(
            "RGBA",
            (int(total_w + pad * 2), int(max_h + pad * 2 + ascent_pad)),
            (0, 0, 0, 0),
        )
        ld = ImageDraw.Draw(layer)
        x = pad
        y = pad + ascent_pad // 2
        # ink densify: draw twice with 1px offset at high ss for crisper dark glyphs
        for ch, w in zip(s, widths):
            ld.text((x, y), ch, font=font, fill=color + (255,))
            if self._ss >= 3:
                ld.text((x + 0.35, y), ch, font=font, fill=color + (210,))
            x += w + track_px
        return layer

    def text(
        self,
        xy,
        s,
        *,
        size=None,
        color=None,
        anchor="mm",
        role=None,
        tracking=None,
        count_label=True,
        rotate=0.0,
    ):
        if count_label:
            self._label_count += 1
        logical_size = size if size is not None else self.style.label_size
        sz = self.sn(logical_size)
        font = load_font(sz, role=self._role_for(logical_size, role))
        c = color or self.style.black
        track_px = (self.style.tracking if tracking is None else tracking) * self._ss
        layer = self._compose_string_layer(s, font, c, track_px)
        if abs(rotate) > 0.05:
            layer = layer.rotate(rotate, resample=Image.Resampling.BICUBIC, expand=True)
        cx, cy = self.sx(xy[0]), self.sy(xy[1])
        lw, lh = layer.size
        ax, ay = anchor[0], anchor[1] if len(anchor) > 1 else "m"
        ox = {"l": 0, "m": lw / 2, "r": lw}.get(ax, lw / 2)
        oy = {"t": 0, "m": lh / 2, "b": lh}.get(ay, lh / 2)
        self.img.paste(layer, (int(cx - ox), int(cy - oy)), layer)

    def label(
        self,
        xy,
        s,
        *,
        size=None,
        color=None,
        bg=None,
        border=None,
        role=None,
        rotate=None,
        count_label=True,
        tilt=False,
    ):
        """Rounded annotation chip with tracking + soft shadow."""
        if count_label:
            self._label_count += 1
        st = self.style
        logical_size = size if size is not None else st.label_size
        sz = self.sn(logical_size)
        font = load_font(sz, role=self._role_for(logical_size, role or "body"))
        c = color or st.black
        bg = bg if bg is not None else st.chip
        border = border if border is not None else st.chip_border
        track_px = st.tracking * self._ss

        text_layer = self._compose_string_layer(s, font, c, track_px)
        tw0, th0 = text_layer.size
        pad_x = self.sn(st.chip_pad_x)
        pad_y = self.sn(st.chip_pad_y)
        tw = tw0 + pad_x
        th = th0 + pad_y
        layer = Image.new("RGBA", (tw + self.sn(8), th + self.sn(8)), (0, 0, 0, 0))
        # shadow
        shadow = Image.new("RGBA", layer.size, (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        sd.rounded_rectangle(
            (self.sn(3), self.sn(4), tw + self.sn(2), th + self.sn(3)),
            radius=self.sn(st.chip_radius),
            fill=(0, 0, 0, 32),
        )
        shadow = shadow.filter(ImageFilter.GaussianBlur(radius=max(1.2, 0.7 * self._ss)))
        layer.alpha_composite(shadow)
        ld = ImageDraw.Draw(layer)
        ld.rounded_rectangle(
            (self.sn(1), self.sn(1), tw - 1, th - 1),
            radius=self.sn(st.chip_radius),
            fill=bg + (252,),
            outline=border + (255,),
            width=max(1, self.sn(1.35)),
        )
        # paste text centered in chip
        ox = (tw - tw0) // 2
        oy = (th - th0) // 2 - self.sn(0.5)
        layer.alpha_composite(text_layer, (ox, oy))

        rot = rotate
        if rot is None:
            rot = st.annot_tilt if tilt else 0.0
        if abs(rot) > 0.05:
            layer = layer.rotate(rot, resample=Image.Resampling.BICUBIC, expand=True)

        cx, cy = self.sx(xy[0]), self.sy(xy[1])
        lw, lh = layer.size
        self.img.paste(layer, (int(cx - lw / 2), int(cy - lh / 2)), layer)

    def x_mark(self, x0, y0, x1, y1, *, color=None, width=3.8):
        c = color or self.style.red
        self.line((x0, y0), (x1, y1), color=c, width=width, seed=90)
        self.line((x1, y0), (x0, y1), color=c, width=width, seed=91)

    def finish(self) -> Image.Image:
        if self._ss == 1:
            return self.img
        # multi-step downscale for glyph edges
        img = self.img
        w, h = self.style.width, self.style.height
        while img.size[0] > w * 2:
            nw = max(w, img.size[0] // 2)
            nh = max(h, img.size[1] // 2)
            img = img.resize((nw, nh), Image.Resampling.LANCZOS)
        if img.size != (w, h):
            img = img.resize((w, h), Image.Resampling.LANCZOS)
        return img

    def save(self, path, *, quality=94) -> Path:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        out = self.finish()
        if path.suffix.lower() in {".jpg", ".jpeg"}:
            out.save(path, quality=quality, optimize=True, subsampling=0)
        else:
            out.save(path)
        return path
