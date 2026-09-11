#!/usr/bin/env python3
"""
Generate PS Vita/PSTV LiveArea presentation assets from authentic
C-Dogs SDL repository artwork only.

Nearest-neighbor scaling only. No invented/AI art.
Final packaged PNGs are indexed (PNG-8, <=256 colors), opaque.

Usage (from repository root):
  python3 scripts/generate_vita_livearea.py

Outputs:
  vita_pkg/sce_sys/icon0.png
  vita_pkg/sce_sys/pic0.png
  vita_pkg/sce_sys/livearea/contents/{bg0,startup}.png
  vita_pkg/sce_sys/livearea/contents/template.xml
  vita_pkg/previews/{icon0,pic0,bg0,startup}.png
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT_SYS = ROOT / "vita_pkg" / "sce_sys"
OUT_CONTENTS = OUT_SYS / "livearea" / "contents"
OUT_PREVIEWS = ROOT / "vita_pkg" / "previews"

# --- Source paths (repository) ---
SRC_LOGO = ROOT / "graphics" / "logo.png"
SRC_BIGPANEL = ROOT / "graphics" / "bigpanel.png"

# --- Destination sizes (Vita LiveArea / sample conventions) ---
ICON_SIZE = (128, 128)
PIC0_SIZE = (960, 544)
BG_SIZE = (840, 500)
STARTUP_SIZE = (280, 158)

# Logo is 80x51.
ICON_LOGO_SCALE = 1  # 80x51 on 128x128 → generous bubble margins
PIC0_LOGO_SCALE = 8  # 640x408 on 960x544
# LiveArea bg0: logo NN ×6, slight upward bias (clear of centered a1 gate)
BG_LOGO_SCALE = 6  # 480x306
BG_LOGO_Y_BIAS = -20  # pixels relative to vertical center (negative = up)

# Flatten alpha onto this opaque fill for Vita-safe opaque PNGs
OPAQUE_BG = (0, 0, 0)

TEMPLATE_XML = """\
<?xml version="1.0" encoding="utf-8"?>\r
\r
<livearea style="a1" format-ver="01.00" content-rev="1">\r
\t<livearea-background>\r
\t\t<image>bg0.png</image>\r
\t</livearea-background>\r
\r
\t<gate>\r
\t\t<startup-image>startup.png</startup-image>\r
\t</gate>\r
</livearea>\r
"""

OBSOLETE = (
	OUT_CONTENTS / "bg.png",
	OUT_CONTENTS / "logo0.png",
	OUT_PREVIEWS / "bg.png",
	OUT_PREVIEWS / "logo0.png",
)


def nn_scale(im: Image.Image, factor: int) -> Image.Image:
	if factor == 1:
		return im.copy()
	w, h = im.size
	return im.resize((w * factor, h * factor), Image.Resampling.NEAREST)


def ensure_rgba(im: Image.Image) -> Image.Image:
	if im.mode == "RGBA":
		return im
	return im.convert("RGBA")


def black_to_alpha(im: Image.Image, threshold: int = 8) -> Image.Image:
	"""Treat near-black as transparent (logo on black)."""
	rgba = ensure_rgba(im)
	pixels = rgba.load()
	w, h = rgba.size
	for y in range(h):
		for x in range(w):
			r, g, b, a = pixels[x, y]
			if r <= threshold and g <= threshold and b <= threshold:
				pixels[x, y] = (r, g, b, 0)
	return rgba


def flatten_opaque(im: Image.Image, fill=OPAQUE_BG) -> Image.Image:
	rgba = ensure_rgba(im)
	base = Image.new("RGB", rgba.size, fill)
	base.paste(rgba, mask=rgba.split()[3])
	return base


def to_vita_png8(im: Image.Image) -> Image.Image:
	"""Vita-safe indexed PNG-8: opaque, <=256 colors, no alpha."""
	rgb = flatten_opaque(im) if im.mode in ("RGBA", "LA", "PA") else im.convert("RGB")
	return rgb.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)


def tile_fill(canvas: Image.Image, tile: Image.Image) -> None:
	tw, th = tile.size
	cw, ch = canvas.size
	for y in range(0, ch, th):
		for x in range(0, cw, tw):
			canvas.alpha_composite(tile, (x, y))


def panel_with_logo(
	size: tuple[int, int],
	logo_scale: int,
	y_bias: int = 0,
) -> Image.Image:
	"""Tiled bigpanel + authentic logo (loading-screen language)."""
	canvas = Image.new("RGBA", size, (0, 0, 0, 255))
	tile_fill(canvas, ensure_rgba(Image.open(SRC_BIGPANEL)))
	logo = black_to_alpha(nn_scale(Image.open(SRC_LOGO), logo_scale))
	lw, lh = logo.size
	cw, ch = size
	if lw > cw or lh > ch:
		raise SystemExit(
			f"logo {lw}x{lh} (scale x{logo_scale}) does not fit canvas {cw}x{ch}"
		)
	lx = (cw - lw) // 2
	ly = (ch - lh) // 2 + y_bias
	if ly < 0 or ly + lh > ch:
		raise SystemExit(f"logo y_bias {y_bias} places logo outside canvas")
	canvas.alpha_composite(logo, (lx, ly))
	return canvas


def make_icon() -> Image.Image:
	"""128x128 bubble: bigpanel + centered logo (native size for margins)."""
	return to_vita_png8(panel_with_logo(ICON_SIZE, ICON_LOGO_SCALE))


def make_pic0() -> Image.Image:
	"""960x544 full-screen launch splash."""
	return to_vita_png8(panel_with_logo(PIC0_SIZE, PIC0_LOGO_SCALE))


def make_startup() -> Image.Image:
	"""Gate image: bigpanel + logo NN ×2."""
	return to_vita_png8(panel_with_logo(STARTUP_SIZE, 2))


def make_bg() -> Image.Image:
	"""LiveArea bg0: tiled bigpanel + logo NN ×6 with slight upward bias."""
	return to_vita_png8(panel_with_logo(BG_SIZE, BG_LOGO_SCALE, BG_LOGO_Y_BIAS))


def save_png8(im: Image.Image, path: Path) -> None:
	if im.mode != "P":
		raise SystemExit(f"expected indexed mode P for {path}, got {im.mode}")
	if "transparency" in im.info:
		raise SystemExit(f"unexpected transparency key on {path}")
	path.parent.mkdir(parents=True, exist_ok=True)
	im.save(path, format="PNG", optimize=False, compress_level=9)
	print(f"wrote {path.relative_to(ROOT)} {im.size} {im.mode}")


def write_template(path: Path) -> None:
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_bytes(TEMPLATE_XML.encode("utf-8"))
	print(f"wrote {path.relative_to(ROOT)} ({len(TEMPLATE_XML.encode('utf-8'))} bytes, CRLF)")


def main() -> int:
	for required in (SRC_LOGO, SRC_BIGPANEL):
		if not required.is_file():
			print(f"missing source: {required}", file=sys.stderr)
			return 1

	OUT_CONTENTS.mkdir(parents=True, exist_ok=True)
	OUT_PREVIEWS.mkdir(parents=True, exist_ok=True)

	for stale in OBSOLETE:
		if stale.is_file():
			stale.unlink()
			print(f"removed obsolete {stale.relative_to(ROOT)}")

	icon = make_icon()
	pic0 = make_pic0()
	startup = make_startup()
	bg = make_bg()

	save_png8(icon, OUT_SYS / "icon0.png")
	save_png8(pic0, OUT_SYS / "pic0.png")
	save_png8(bg, OUT_CONTENTS / "bg0.png")
	save_png8(startup, OUT_CONTENTS / "startup.png")
	write_template(OUT_CONTENTS / "template.xml")

	save_png8(icon, OUT_PREVIEWS / "icon0.png")
	save_png8(pic0, OUT_PREVIEWS / "pic0.png")
	save_png8(bg, OUT_PREVIEWS / "bg0.png")
	save_png8(startup, OUT_PREVIEWS / "startup.png")

	print("done.")
	return 0


if __name__ == "__main__":
	sys.exit(main())
