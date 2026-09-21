"""「花火 Flower-Burst」fancy clock digit font generator.

Project-original stylized ``HH:mm`` digit bitmaps for the Frame clock face
(UI-004). All glyph geometry is defined in normalized units (fractions of the
digit height) so the same design rasterizes crisply at any target size; the
canonical committed asset set is 48x80 digits plus one 20x80 colon.

Design highlights:
- Flower element: free stroke terminals blossom into 5-petal flowers.
- Explosion element: outer corner anchors emit 3-ray bursts.
- Level-of-detail (LOD) rules degrade decoration at small sizes.
- Region-aware composition clips ink strictly inside a target rectangle.

Pure stdlib; deterministic byte-for-byte regeneration.
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from collections.abc import Sequence
from pathlib import Path
from typing import Literal

# --- Normalized design constants (fractions of digit height H unless noted) ---

DIGIT_ASPECT = 3 / 5  # digit cell width / height (48/80)
COLON_WIDTH = 1 / 4  # colon cell width / digit height (20/80)
STROKE = 0.10  # stroke weight
CAP_TOP = 0.075
BASELINE = 0.925
BLOSSOM_CORE = 0.025  # flower core radius
PETAL_OFFSET = 0.02  # petal base distance from flower center
PETAL_LEN = 0.07  # petal length
PETAL_HALF = 0.029  # petal half-width at base
SEED_R = 0.05  # terminal seed-dot radius
RAY_LEN = 0.11
RAY_W = 0.046
RAY_FAN = 35.0  # degrees between adjacent rays
COMPOSE_MARGIN = 0.05  # fit margin inside a target region

LOD_FULL_MIN = 48  # digit height >= this: full flower-burst detail
LOD_SIMPLE_MIN = 24  # >= this: 4 petals, no rays; below: skeleton only

CANONICAL_HEIGHT = 80
PREVIEW_W, PREVIEW_H = 400, 300
LADDER_HEIGHTS = (96, 64, 48, 32, 24, 16)
PREVIEW_FILES = (
    "preview_clock_400x300.png",
    "preview_clock_subregion.png",
    "glyph_sheet.png",
    "preview_scale_ladder.png",
)

Vec = tuple[float, float]

Op = (
    tuple[Literal["seg"], Vec, Vec]
    | tuple[Literal["quad"], Vec, Vec, Vec]
    | tuple[Literal["cubic"], Vec, Vec, Vec, Vec]
    | tuple[Literal["ellipse"], Vec, float, float]
    | tuple[Literal["blossom"], Vec]
    | tuple[Literal["seed"], Vec]
    | tuple[Literal["burst"], Vec, float]
)

# --- Geometry DSL: (x, y) are fractions of cell width / cell height ---


def _seg(p0: Vec, p1: Vec) -> Op:
    return ("seg", p0, p1)


def _quad(p0: Vec, c: Vec, p1: Vec) -> Op:
    return ("quad", p0, c, p1)


def _cubic(p0: Vec, c1: Vec, c2: Vec, p1: Vec) -> Op:
    return ("cubic", p0, c1, c2, p1)


def _ellipse(c: Vec, rx: float, ry: float) -> Op:
    return ("ellipse", c, rx, ry)


def _blossom(p: Vec) -> Op:
    return ("blossom", p)


def _seed(p: Vec) -> Op:
    return ("seed", p)


def _burst(p: Vec, direction_deg: float) -> Op:
    return ("burst", p, direction_deg)


# Shared x positions (fractions of cell width).
_L, _R = 0.23, 0.77
_STEM1 = 0.5625

GLYPHS: dict[str, list[Op]] = {
    "0": [
        _ellipse((0.5, 0.5), 0.27, 0.4125),
        _burst((0.225, 0.19), 255.0),
        _burst((0.74, 0.81), 45.0),
    ],
    "1": [
        _seg((_STEM1, 0.125), (_STEM1, BASELINE)),
        _seg((0.29, 0.33), (_STEM1, 0.125)),
        _seg((0.354, BASELINE), (0.6875, BASELINE)),
        _blossom((0.29, 0.33)),
        _burst((0.74, 0.19), 315.0),
        _burst((0.25, 0.79), 135.0),
    ],
    "2": [
        _quad((0.25, 0.25), (0.5, 0.05), (0.75, 0.25)),
        _seg((0.75, 0.25), (0.27, BASELINE)),
        _seg((0.27, BASELINE), (0.75, BASELINE)),
        _blossom((0.25, 0.25)),
        _burst((0.74, 0.19), 315.0),
        _burst((0.72, 0.78), 5.0),
    ],
    "3": [
        _cubic((0.27, 0.20), (0.27, 0.05), (0.75, 0.05), (0.75, 0.225)),
        _cubic((0.75, 0.225), (0.75, 0.36), (0.58, 0.44), (0.48, 0.4875)),
        _cubic((0.48, 0.5125), (0.15, 0.56), (0.15, 0.86), (0.5, BASELINE)),
        _cubic((0.5, BASELINE), (0.79, BASELINE), (0.81, 0.75), (0.77, 0.6875)),
        _blossom((0.27, 0.20)),
        _blossom((0.77, 0.6875)),
        _burst((0.72, 0.82), 0.0),
    ],
    "4": [
        _seg((0.708, 0.125), (0.708, BASELINE)),
        _seg((0.708, 0.125), (0.229, 0.60)),
        _seg((0.229, 0.60), (0.8125, 0.60)),
        _blossom((0.229, 0.60)),
        _blossom((0.708, 0.125)),
        _seed((0.8125, 0.60)),
        _burst((0.24, 0.17), 250.0),
        _burst((0.25, 0.84), 160.0),
    ],
    "5": [
        _seg((0.25, 0.125), (0.708, 0.125)),
        _seg((0.25, 0.125), (0.25, 0.525)),
        _cubic((0.25, 0.525), (0.17, 0.60), (0.17, 0.90), (0.4, BASELINE)),
        _cubic((0.4, BASELINE), (0.66, 0.94), (0.77, 0.82), (0.77, 0.70)),
        _blossom((0.708, 0.125)),
        _blossom((0.77, 0.70)),
        _burst((0.72, 0.82), 5.0),
    ],
    "6": [
        _cubic((0.75, 0.165), (0.25, 0.05), (0.19, 0.25), (0.229, 0.475)),
        _seg((0.229, 0.475), (0.229, 0.55)),
        _cubic((0.229, 0.55), (0.19, 0.83), (0.36, 0.95), (0.52, BASELINE)),
        _cubic((0.52, BASELINE), (0.75, BASELINE), (0.77, 0.82), (0.75, 0.65)),
        _blossom((0.75, 0.165)),
        _blossom((0.75, 0.65)),
        _burst((0.72, 0.82), 5.0),
    ],
    "7": [
        _seg((_L, 0.125), (_R, 0.125)),
        _seg((_R, 0.125), (0.4375, BASELINE)),
        _blossom((_L, 0.125)),
        _seed((0.4375, BASELINE)),
        _burst((0.25, 0.845), 130.0),
        _burst((0.74, 0.79), 40.0),
    ],
    "8": [
        _ellipse((0.5, 0.275), 0.21, 0.19),
        _ellipse((0.5, 0.6875), 0.27, 0.2375),
        _burst((0.24, 0.23), 245.0),
        _burst((0.74, 0.77), 65.0),
    ],
    "9": [
        _cubic((0.29, 0.47), (0.16, 0.33), (0.28, 0.075), (0.55, 0.075)),
        _cubic((0.55, 0.075), (0.74, 0.075), (0.79, 0.24), (0.77, 0.40)),
        _cubic((0.77, 0.40), (0.75, 0.62), (0.66, 0.85), (0.58, BASELINE)),
        _blossom((0.29, 0.47)),
        _seed((0.58, BASELINE)),
        _burst((0.225, 0.19), 255.0),
        _burst((0.74, 0.79), 40.0),
    ],
    ":": [
        _blossom((0.5, 1 / 3)),
        _blossom((0.5, 2 / 3)),
    ],
}

GLYPH_ORDER = ("0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":")


# --- 1-bit bitmap canvas ---


class Bitmap:
    """A 1-bit ink map; 1 = black ink, 0 = white."""

    def __init__(self, width: int, height: int, ink: bool = False) -> None:
        self.width = width
        self.height = height
        fill = 1 if ink else 0
        self.bits = bytearray([fill]) * (width * height)

    def set(self, x: int, y: int) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            self.bits[y * self.width + x] = 1

    def get(self, x: int, y: int) -> int:
        return self.bits[y * self.width + x]

    def ink_count(self) -> int:
        return sum(self.bits)

    def ink_bounds(self) -> tuple[int, int, int, int] | None:
        xs = [i for i in range(self.width) for j in range(self.height) if self.get(i, j)]
        ys = [j for i in range(self.width) for j in range(self.height) if self.get(i, j)]
        if not xs:
            return None
        return min(xs), min(ys), max(xs), max(ys)


# --- Rasterizer primitives (pure functions over pixel space) ---


def _lerp(a: Vec, b: Vec, t: float) -> Vec:
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def _quad_points(p0: Vec, c: Vec, p1: Vec, n: int = 24) -> list[Vec]:
    return [
        (
            (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t**2 * p1[0],
            (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t**2 * p1[1],
        )
        for t in (i / n for i in range(n + 1))
    ]


def _cubic_points(p0: Vec, c1: Vec, c2: Vec, p1: Vec, n: int = 32) -> list[Vec]:
    return [
        (
            (1 - t) ** 3 * p0[0]
            + 3 * (1 - t) ** 2 * t * c1[0]
            + 3 * (1 - t) * t**2 * c2[0]
            + t**3 * p1[0],
            (1 - t) ** 3 * p0[1]
            + 3 * (1 - t) ** 2 * t * c1[1]
            + 3 * (1 - t) * t**2 * c2[1]
            + t**3 * p1[1],
        )
        for t in (i / n for i in range(n + 1))
    ]


def _dist_to_segment(px: float, py: float, a: Vec, b: Vec) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    length_sq = dx * dx + dy * dy
    if length_sq == 0:
        return math.hypot(px - a[0], py - a[1])
    t = max(0.0, min(1.0, ((px - a[0]) * dx + (py - a[1]) * dy) / length_sq))
    return math.hypot(px - (a[0] + t * dx), py - (a[1] + t * dy))


def _draw_polyline(bm: Bitmap, pts: list[Vec], width: float) -> None:
    half = width / 2
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0 = max(0, int(math.floor(min(xs) - half)))
    x1 = min(bm.width - 1, int(math.ceil(max(xs) + half)))
    y0 = max(0, int(math.floor(min(ys) - half)))
    y1 = min(bm.height - 1, int(math.ceil(max(ys) + half)))
    for j in range(y0, y1 + 1):
        for i in range(x0, x1 + 1):
            px, py = i + 0.5, j + 0.5
            for k in range(len(pts) - 1):
                if _dist_to_segment(px, py, pts[k], pts[k + 1]) <= half:
                    bm.set(i, j)
                    break


def _draw_circle(bm: Bitmap, c: Vec, r: float) -> None:
    x0 = max(0, int(math.floor(c[0] - r)))
    x1 = min(bm.width - 1, int(math.ceil(c[0] + r)))
    y0 = max(0, int(math.floor(c[1] - r)))
    y1 = min(bm.height - 1, int(math.ceil(c[1] + r)))
    for j in range(y0, y1 + 1):
        for i in range(x0, x1 + 1):
            if math.hypot(i + 0.5 - c[0], j + 0.5 - c[1]) <= r:
                bm.set(i, j)


def _draw_petal(bm: Bitmap, c: Vec, rot_deg: float, scale: float) -> None:
    """One teardrop petal pointing outward along ``rot_deg`` from center ``c``."""
    ang = math.radians(rot_deg)
    dx, dy = math.cos(ang), math.sin(ang)
    ux, uy = -dy, dx
    v0 = PETAL_OFFSET * scale
    length = PETAL_LEN * scale
    half = PETAL_HALF * scale
    tip = (c[0] + dx * (v0 + length), c[1] + dy * (v0 + length))
    x0 = max(0, int(math.floor(min(c[0], tip[0]) - half)))
    x1 = min(bm.width - 1, int(math.ceil(max(c[0], tip[0]) + half)))
    y0 = max(0, int(math.floor(min(c[1], tip[1]) - half)))
    y1 = min(bm.height - 1, int(math.ceil(max(c[1], tip[1]) + half)))
    for j in range(y0, y1 + 1):
        for i in range(x0, x1 + 1):
            rx = (i + 0.5 - c[0]) * dx + (j + 0.5 - c[1]) * dy
            ry = (i + 0.5 - c[0]) * ux + (j + 0.5 - c[1]) * uy
            if v0 - 0.5 <= rx <= v0 + length:
                s = (rx - v0) / length
                if 0 <= s <= 1 and abs(ry) <= half * (1 - s * s) ** 0.35 + 0.3:
                    bm.set(i, j)


def _draw_blossom(bm: Bitmap, c: Vec, rot_deg: float, scale: float, lod: int) -> None:
    petals = 5 if lod >= 2 else 4
    core = BLOSSOM_CORE * scale * (1.25 if lod == 1 else 1.0)
    if lod < 1:
        _draw_circle(bm, c, SEED_R * scale)
        return
    _draw_circle(bm, c, core)
    for k in range(petals):
        _draw_petal(bm, c, rot_deg + k * 360.0 / petals, scale)


def _draw_burst(bm: Bitmap, anchor: Vec, direction_deg: float, scale: float) -> None:
    length = RAY_LEN * scale
    width = RAY_W * scale
    for delta in (-RAY_FAN, 0.0, RAY_FAN):
        ang = math.radians(direction_deg + delta)
        end = (anchor[0] + math.cos(ang) * length, anchor[1] + math.sin(ang) * length)
        _draw_polyline(bm, [anchor, end], width)


def _draw_ellipse(bm: Bitmap, c: Vec, rx: float, ry: float, width: float) -> None:
    pts: list[Vec] = []
    n = 96
    for i in range(n + 1):
        t = 2 * math.pi * i / n
        pts.append((c[0] + rx * math.cos(t), c[1] + ry * math.sin(t)))
    _draw_polyline(bm, pts, width)


# --- Glyph rendering (normalized geometry -> pixels at any size) ---


def cell_size(ch: str, digit_height: int) -> tuple[int, int]:
    """Return (width, height) of a glyph cell for the requested digit height."""
    if ch == ":":
        return max(1, round(digit_height * COLON_WIDTH)), digit_height
    return max(1, round(digit_height * DIGIT_ASPECT)), digit_height


def lod_for(digit_height: int) -> int:
    if digit_height >= LOD_FULL_MIN:
        return 2
    if digit_height >= LOD_SIMPLE_MIN:
        return 1
    return 0


def render_glyph(ch: str, digit_height: int) -> Bitmap:
    """Rasterize one glyph at the requested digit height (>= 1 px)."""
    if ch not in GLYPHS:
        raise ValueError(f"unsupported glyph: {ch!r}")
    width, height = cell_size(ch, digit_height)
    bm = Bitmap(width, height)
    lod = lod_for(digit_height)

    def to_px(p: Vec) -> Vec:
        return (p[0] * width, p[1] * height)

    for op in GLYPHS[ch]:
        match op:
            case ("seg", p0, p1):
                _draw_polyline(bm, [to_px(p0), to_px(p1)], STROKE * digit_height)
            case ("quad", p0, c, p1):
                pts = _quad_points(to_px(p0), to_px(c), to_px(p1))
                _draw_polyline(bm, pts, STROKE * digit_height)
            case ("cubic", p0, c1, c2, p1):
                pts = _cubic_points(to_px(p0), to_px(c1), to_px(c2), to_px(p1))
                _draw_polyline(bm, pts, STROKE * digit_height)
            case ("ellipse", c, rx, ry):
                _draw_ellipse(bm, to_px(c), rx * width, ry * height, STROKE * digit_height)
            case ("blossom", p):
                px = to_px(p)
                outward = math.degrees(
                    math.atan2(px[1] - height / 2, (px[0] - width / 2) * height / width)
                )
                _draw_blossom(bm, px, outward, digit_height, lod)
            case ("seed", p):
                _draw_circle(bm, to_px(p), SEED_R * digit_height)
            case ("burst", p, direction):
                if lod >= 2:
                    _draw_burst(bm, to_px(p), direction, digit_height)
    return bm


# --- Region-aware composition ---


def _text_group_width(text: str, digit_height: int) -> float:
    total = 0.0
    for ch in text:
        total += cell_size(ch, digit_height)[0]
    return total


def fit_height(rect_w: int, rect_h: int, text: str) -> int:
    """Largest digit height whose text group fits the rect with margins."""
    margin_x = rect_w * COMPOSE_MARGIN
    margin_y = rect_h * COMPOSE_MARGIN
    for h in range(min(rect_h, 512), 0, -1):
        if _text_group_width(text, h) <= rect_w - 2 * margin_x and h <= rect_h - 2 * margin_y:
            return h
    return 1


def compose_text(target: Bitmap, rect: tuple[int, int, int, int], text: str) -> None:
    """Draw ``text`` glyphs fitted and centered inside ``rect``; ink never leaks."""
    x, y, w, h = rect
    digit_height = fit_height(w, h, text)
    group_w = round(_text_group_width(text, digit_height))
    cursor = x + (w - group_w) // 2
    base_y = y + (h - digit_height) // 2
    for ch in text:
        glyph = render_glyph(ch, digit_height)
        for gy in range(glyph.height):
            for gx in range(glyph.width):
                tx, ty = cursor + gx, base_y + gy
                if glyph.get(gx, gy) and x <= tx < x + w and y <= ty < y + h:
                    target.set(tx, ty)
        cursor += glyph.width


# --- Encoders ---


def pbm_bytes(bm: Bitmap) -> bytes:
    """PBM P4 image bytes: 1 = black ink, rows padded to byte boundary."""
    header = f"P4\n{bm.width} {bm.height}\n".encode("ascii")
    row_bytes = (bm.width + 7) // 8
    payload = bytearray(row_bytes * bm.height)
    for j in range(bm.height):
        for i in range(bm.width):
            if bm.get(i, j):
                payload[j * row_bytes + i // 8] |= 0x80 >> (i % 8)
    return header + bytes(payload)


def raw_bytes(bm: Bitmap) -> bytes:
    """Headerless packed 1-bit MSB-first payload for firmware/MPB use."""
    return pbm_bytes(bm)[len(f"P4\n{bm.width} {bm.height}\n".encode("ascii")) :]


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def png_bytes(bm: Bitmap, scale: int = 1) -> bytes:
    """Deterministic grayscale-8 PNG; ink renders black on white."""
    w, h = bm.width * scale, bm.height * scale
    scanlines = bytearray()
    for j in range(h):
        scanlines.append(0)  # filter type None
        src_y = j // scale
        for i in range(w):
            scanlines.append(0 if bm.get(i // scale, src_y) else 255)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
        + _png_chunk(b"IEND", b"")
    )


# --- Previews ---


def _dashed_rect(bm: Bitmap, rect: tuple[int, int, int, int], dash: int = 4) -> None:
    x, y, w, h = rect
    run = 2 * dash

    def dash_h(yy: int, x0: int, x1: int) -> None:
        xx = x0
        while xx < x1:
            for k in range(min(dash, x1 - xx)):
                bm.set(xx + k, yy)
            xx += run

    def dash_v(xx: int, y0: int, y1: int) -> None:
        yy = y0
        while yy < y1:
            for k in range(min(dash, y1 - yy)):
                bm.set(xx, yy + k)
            yy += run

    dash_h(y, x, x + w)
    dash_h(y + h - 1, x, x + w)
    dash_v(x, y, y + h)
    dash_v(x + w - 1, y, y + h)


def render_full_preview() -> Bitmap:
    """Canonical clock-face layout on the 400x300 logical canvas."""
    bm = Bitmap(PREVIEW_W, PREVIEW_H)
    text = "12:34"
    group_w = round(_text_group_width(text, CANONICAL_HEIGHT))
    cursor = (PREVIEW_W - group_w) // 2
    for ch in text:
        cell = render_glyph(ch, CANONICAL_HEIGHT)
        for gy in range(cell.height):
            for gx in range(cell.width):
                if cell.get(gx, gy):
                    bm.set(cursor + gx, 70 + gy)
        cursor += cell.width
    # UI-005 placeholder strip for the standard-font date line (not an asset).
    strip_w, strip_h = 224, 25
    x0 = (PREVIEW_W - strip_w) // 2
    y0 = 70 + CANONICAL_HEIGHT + 24
    for j in range(strip_h):
        for i in range(strip_w):
            bm.set(x0 + i, y0 + j)
    return bm


def render_subregion_preview() -> Bitmap:
    """Clock composed into an off-center sub-region with a dashed outline."""
    bm = Bitmap(PREVIEW_W, PREVIEW_H)
    region = (140, 100, 200, 120)
    _dashed_rect(bm, region)
    compose_text(bm, region, "12:34")
    return bm


def render_glyph_sheet() -> Bitmap:
    """All 11 canonical glyphs in reading order for visual QA."""
    pad, gap = 6, 6
    cells = [render_glyph(ch, CANONICAL_HEIGHT) for ch in GLYPH_ORDER]
    width = pad * 2 + sum(c.width for c in cells) + gap * (len(cells) - 1)
    height = CANONICAL_HEIGHT + pad * 2
    bm = Bitmap(width, height)
    cursor = pad
    for cell in cells:
        for gy in range(cell.height):
            for gx in range(cell.width):
                if cell.get(gx, gy):
                    bm.set(cursor + gx, pad + gy)
        cursor += cell.width + gap
    return bm


def render_scale_ladder() -> Bitmap:
    """``12:34`` at decreasing digit heights to show LOD degradation."""
    pad, gap = 8, 10
    rows = [render_glyph_list_row(h) for h in LADDER_HEIGHTS]
    width = pad * 2 + max(r.width for r in rows)
    height = pad * 2 + sum(r.height for r in rows) + gap * (len(rows) - 1)
    bm = Bitmap(width, height)
    y = pad
    for row in rows:
        for j in range(row.height):
            for i in range(row.width):
                if row.get(i, j):
                    bm.set(pad + i, y + j)
        y += row.height + gap
    return bm


def render_glyph_list_row(digit_height: int, text: str = "12:34") -> Bitmap:
    cells = [render_glyph(ch, digit_height) for ch in text]
    width = sum(c.width for c in cells)
    bm = Bitmap(width, digit_height)
    cursor = 0
    for cell in cells:
        for gy in range(cell.height):
            for gx in range(cell.width):
                if cell.get(gx, gy):
                    bm.set(cursor + gx, gy)
        cursor += cell.width
    return bm


# --- File output ---


def glyph_filename(ch: str) -> str:
    return "colon.pbm" if ch == ":" else f"digit_{ch}.pbm"


def write_canonical_set(output_dir: Path) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for ch in GLYPH_ORDER:
        path = output_dir / glyph_filename(ch)
        path.write_bytes(pbm_bytes(render_glyph(ch, CANONICAL_HEIGHT)))
        written.append(path)
    return written


def write_scaled_set(output_dir: Path, digit_height: int) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for ch in GLYPH_ORDER:
        path = output_dir / glyph_filename(ch)
        path.write_bytes(pbm_bytes(render_glyph(ch, digit_height)))
        written.append(path)
    return written


def write_previews(output_dir: Path) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    renderers = {
        "preview_clock_400x300.png": render_full_preview,
        "preview_clock_subregion.png": render_subregion_preview,
        "glyph_sheet.png": render_glyph_sheet,
        "preview_scale_ladder.png": render_scale_ladder,
    }
    written: list[Path] = []
    for name in PREVIEW_FILES:
        path = output_dir / name
        path.write_bytes(png_bytes(renderers[name](), 2))
        written.append(path)
    return written


def emit_raw(output_dir: Path, digit_height: int = CANONICAL_HEIGHT) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for ch in GLYPH_ORDER:
        stem = "colon" if ch == ":" else f"digit_{ch}"
        path = output_dir / f"{stem}.bin"
        path.write_bytes(raw_bytes(render_glyph(ch, digit_height)))
        written.append(path)
    return written


# --- CLI ---


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the Frame「花火 Flower-Burst」clock digit assets"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("_doc/design/clock-digits"),
        help="directory for canonical PBM glyphs and PNG previews",
    )
    parser.add_argument(
        "--glyph-height",
        type=int,
        default=None,
        help="emit a scaled glyph set at this digit height instead of the canonical set",
    )
    parser.add_argument(
        "--emit-raw",
        type=Path,
        default=None,
        help="also emit headerless packed 1-bit .bin payloads to this directory",
    )
    args = parser.parse_args(argv)

    if args.glyph_height is not None and args.glyph_height < 1:
        parser.error("--glyph-height must be >= 1")

    if args.glyph_height is not None:
        written = write_scaled_set(args.output, args.glyph_height)
    else:
        written = write_canonical_set(args.output)
        written += write_previews(args.output)

    raw: list[Path] = []
    if args.emit_raw is not None:
        raw = emit_raw(args.emit_raw, args.glyph_height or CANONICAL_HEIGHT)

    print(f"wrote {len(written)} glyph/preview files to {args.output}")
    if raw:
        print(f"wrote {len(raw)} raw .bin payloads to {args.emit_raw}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
