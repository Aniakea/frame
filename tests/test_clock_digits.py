"""Verification for the「花火 Flower-Burst」clock digit design (UI-004)."""

from pathlib import Path

from frame_tools.clock_digits import (
    CANONICAL_HEIGHT,
    DIGIT_ASPECT,
    GLYPH_ORDER,
    PREVIEW_FILES,
    Bitmap,
    cell_size,
    compose_text,
    fit_height,
    lod_for,
    pbm_bytes,
    png_bytes,
    raw_bytes,
    render_full_preview,
    render_glyph,
    render_subregion_preview,
)

ASSET_DIR = Path("_doc/design/clock-digits")
DIGITS = tuple("0123456789")
CANONICAL_FILES = tuple(f"digit_{ch}.pbm" if ch != ":" else "colon.pbm" for ch in GLYPH_ORDER)


def read_pbm_header(data: bytes) -> tuple[int, int, int]:
    magic, dims, _payload = data.split(b"\n", 2)
    assert magic == b"P4"
    width, height = map(int, dims.split())
    row_bytes = (width + 7) // 8
    return width, height, row_bytes * height


def test_canonical_asset_set_is_exactly_eleven_glyphs() -> None:
    present = {p.name for p in ASSET_DIR.glob("*.pbm")}
    assert present == set(CANONICAL_FILES)
    # No seconds glyph or stray character assets among previews either.
    assert {p.name for p in ASSET_DIR.glob("*.png")} == set(PREVIEW_FILES)


def test_canonical_dimensions_and_payload_sizes() -> None:
    for name in CANONICAL_FILES:
        width, height, payload = read_pbm_header((ASSET_DIR / name).read_bytes())
        expected = (20, 80, 240) if name == "colon.pbm" else (48, 80, 480)
        assert (width, height, payload) == expected, name


def test_committed_files_match_generator() -> None:
    for ch in GLYPH_ORDER:
        name = "colon.pbm" if ch == ":" else f"digit_{ch}.pbm"
        assert (ASSET_DIR / name).read_bytes() == pbm_bytes(render_glyph(ch, CANONICAL_HEIGHT)), (
            name
        )
    assert (ASSET_DIR / "preview_clock_400x300.png").read_bytes() == png_bytes(
        render_full_preview(), 2
    )
    assert (ASSET_DIR / "preview_clock_subregion.png").read_bytes() == png_bytes(
        render_subregion_preview(), 2
    )


def test_raw_payload_equals_pbm_bits() -> None:
    for ch in GLYPH_ORDER:
        glyph = render_glyph(ch, CANONICAL_HEIGHT)
        assert raw_bytes(glyph) == pbm_bytes(glyph).split(b"\n", 2)[2]


def test_every_glyph_has_ink_inside_safe_margins() -> None:
    for ch in GLYPH_ORDER:
        glyph = render_glyph(ch, CANONICAL_HEIGHT)
        bounds = glyph.ink_bounds()
        assert bounds is not None, ch
        x0, y0, x1, y1 = bounds
        assert x0 >= 2 and x1 <= glyph.width - 3, (ch, bounds)
        assert y0 >= 2 and y1 <= glyph.height - 3, (ch, bounds)


def test_digit_ink_fraction_stays_in_documented_band() -> None:
    for ch in DIGITS:
        glyph = render_glyph(ch, CANONICAL_HEIGHT)
        fraction = glyph.ink_count() / (glyph.width * glyph.height)
        assert 0.18 <= fraction <= 0.40, (ch, fraction)
    colon = render_glyph(":", CANONICAL_HEIGHT)
    assert colon.ink_count() / (colon.width * colon.height) >= 0.10


def test_generation_is_deterministic(tmp_path: Path) -> None:
    from frame_tools.clock_digits import write_canonical_set, write_previews

    write_canonical_set(tmp_path)
    write_previews(tmp_path)
    for name in (*CANONICAL_FILES, *PREVIEW_FILES):
        assert (tmp_path / name).read_bytes() == (ASSET_DIR / name).read_bytes(), name


def test_scaled_rendering_keeps_aspect_and_proportions() -> None:
    for height in (96, 70, 48, 32):
        width, got_h = cell_size("5", height)
        assert (width, got_h) == (round(height * DIGIT_ASPECT), height)
        glyph = render_glyph("5", height)
        assert glyph.ink_count() > 0
        assert glyph.width == width and glyph.height == height
    # Colon width tracks digit height (canonical 20 for 80).
    assert cell_size(":", 80) == (20, 80)
    assert cell_size(":", 96) == (24, 96)


def test_lod_bands_match_documented_thresholds() -> None:
    assert lod_for(96) == 2
    assert lod_for(48) == 2
    assert lod_for(47) == 1
    assert lod_for(24) == 1
    assert lod_for(23) == 0
    assert lod_for(16) == 0


def test_region_composition_clips_ink_strictly_inside_rect() -> None:
    canvas = Bitmap(200, 120)
    rect = (37, 23, 111, 67)
    compose_text(canvas, rect, "12:34")
    outside = 0
    inside = 0
    for y in range(canvas.height):
        for x in range(canvas.width):
            if canvas.get(x, y):
                rx, ry, rw, rh = rect
                if rx <= x < rx + rw and ry <= y < ry + rh:
                    inside += 1
                else:
                    outside += 1
    assert outside == 0
    assert inside > 0


def test_fit_height_respects_region_margins() -> None:
    # 2.65 = group width factor of "12:34" (4 digits + colon) per unit height.
    height = fit_height(200, 120, "12:34")
    assert height * 2.65 <= 200 * 0.95
    assert height <= 120 * 0.9
    assert height >= 1
