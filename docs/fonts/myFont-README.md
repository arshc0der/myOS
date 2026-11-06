# myFont — typeface of myOS Horizon

An **original** geometric-humanist sans designed for desktop UIs. Every glyph is drawn from
scratch by the generator in `source/` (no Apple/San Francisco outlines or files were used or copied).
Open apertures, tabular numerals, distinct `1 l I` and `0 O`, eight weights plus a monospace pair.

## What is in the zip

| Path | What |
|---|---|
| `assets/fonts/myFont/` | **The 8 TTFs** — Thin, Light, Regular, Medium, Semibold, Bold, Heavy, Black (the layout you asked for) |
| `assets/fonts/myFontMono/` | Monospace fallback for terminals: Regular, Bold (600-unit cell, slashed zero, serifed `I l 1`) |
| `fonts/otf/`, `fonts/woff/` | Same fonts as OTF (CFF) and WOFF for web previews |
| `bitmap/*.mfb` | Pre-rasterized fonts for the kernel (12/14/16/20/24/32 px; mono 12–24 px), ~240 KB each |
| `os/` | `myfont.h/.c`: freestanding C renderer (no libc, no malloc) + host test |
| `tools/` | `bake_mfb.py` (TTF → .mfb), `mfb2c.py` (.mfb → C array) |
| `specimen/` | 7-page specimen (PNG + `myFont-specimen.pdf`) and `specimen.html` |
| `source/` | The generator: `python3 source/build.py out/` rebuilds every font |
| `docs/INTEGRATION.md` | How to hook it into myOS; `docs/VALIDATION.txt` the checks that were run |

## Coverage
Basic Latin, Latin-1 Supplement (including accented letters, Æ æ Ð ð Þ þ ß Ø ø œ), common punctuation,
currency (`$ € ₹ £ ¥ ¢ ¤`), math (`± × ÷ − ≠ ≤ ≥ ≈ ∞`), arrows (`← ↑ → ↓ ↔`) and UI symbols
(`• … ✓ ✕ ⌘ ⇧ ⌥ ⌃`) — 223 glyphs per font, with kerning (GPOS `kern`).
Devanagari is **not** included yet (see "Honest status").

## Honest status — read this
Verified here:
- All 30 files (8 TTF + 8 OTF + 8 WOFF + mono) load with fontTools, have valid required tables, correct naming
  (family `myFont`, distinct style names, weight classes 100–900), consistent metrics, tabular digits, and every
  mapped glyph renders through FreeType (Pillow) and fontconfig (`fc-scan`). See `docs/VALIDATION.txt`.
- The C renderer compiles with `-ffreestanding -Wall -Wextra -Wpedantic` and renders correctly in a host test
  (kerning, UTF-8, clipping, font fallback).

Not done / limitations:
- **Not tested inside myOS.** GitHub blocked automated access to the repository source, so I could not read your
  `src/gfx/font.c` and nothing was wired into the repo. `docs/INTEGRATION.md` explains the hook-up.
- **Not tested in a second independent font viewer** beyond FreeType/fontconfig (no FontBook/Windows here).
  Open the TTFs in one before shipping.
- **Outlines are polylines (many short straight segments), not hand-tuned Béziers**, and strokes are built from
  overlapping contours (non-zero winding; the TrueType overlap flag is set). Rendering is correct in FreeType,
  stb_truetype and browsers. If a rasterizer uses even-odd fill, run an overlap-removal pass first.
- **No hinting** (gasp set to grayscale smoothing). Text is crisp at 12 px+ in anti-aliased renderers; for 1-bit
  rendering use the baked `.mfb` files.
- **WOFF2 not produced** (the `brotli` module was unavailable); WOFF is provided. `pip install brotli` then
  `fonttools ttLib.woff2 compress file.ttf` makes WOFF2.
- Heavy weights keep counters open but `& @` are the tightest glyphs; a human pass on the Black weight is worthwhile.
- Some Latin-1 letters (Æ æ ð ß) are simple constructions, not refined designs.
- License: nothing chosen. Copyright string says "All rights reserved by the project owner". Pick one
  (OFL-1.1 is usual for fonts; MIT matches your repo) and update the name table in `source/build.py`.

## Rebuild
```
pip install fonttools pillow
python3 source/build.py out        # fonts into out/{ttf,otf,woff}
python3 source/validate.py out     # checks
python3 source/specimen.py out sp  # specimen pages
for f in out/ttf/*.ttf; do python3 tools/bake_mfb.py $f bitmap/$(basename ${f%.ttf}).mfb; done
```
