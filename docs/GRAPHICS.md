# Horizon graphics engine (2.8.0)

Compiled only. Nothing here was run or measured; no before/after numbers exist. Use the overlay
(Settings > Display > Performance overlay) to see real numbers on your machine.

## Done
- **myFont is the system font** (src/gfx/myfont.c from the font package, glue in src/gfx/uifont.c, faces linked by
  src/gfx/fonts_data.S from assets/fonts/*.mfb). `gfx_text` scale 1 = Regular 14 px, 2 = Semibold 20 px, 3 = Bold 24 px,
  4+ = Bold 32 px. All 8 weights + Mono pair are available through `ui_text(..., weight)` (include/uifont.h).
  Kerning, UTF-8 (Latin-1, punctuation, currency, arrows) and Mono fallback come from the package.
- **Faster primitives**: `memcpy/memmove/memset` now copy 32-bit words (window blits used a byte-by-byte `memmove`
  before), `gfx_fill_rect` uses `rep stosl`, integer packed alpha blend (no divisions), faster alpha fills.
- **Anti-aliased rounded corners and circles** (integer coverage per edge pixel).
- **Present only what changed**: a shadow copy of the screen lets `gfx_flush` write only differing pixel spans to the
  frame buffer (a still desktop writes almost nothing).
- **Time-based animations**: window open (240 ms ease-out cubic), dock hop (520 ms), dock magnification use real time
  (TSC based `timer_us()`), not frame counts. "Reduce motion" turns them off.
- **Display capability detection** (src/gfx/display.c): resolution, depth, pitch, Bochs VBE presence, and the physical
  refresh rate **only when measured** from VGA vertical retrace pulses. Otherwise reported as unknown.
  Four separate numbers: physical refresh, target FPS (Auto / 30 / 60 / 90 / 120, limited to the measured rate when
  known), measured FPS, vsync (hardware retrace when it really toggles, otherwise timer pacing).
- **Frame scheduler**: delta time, frame duration, missed-frame count (frames > 1.5 x budget while animating), sleep
  while there is time, then poll input until the deadline (the timer tick is 100 Hz, too coarse to end the wait exactly).
- **Diagnostics**: FPS now/average, render time, present time, dirty rows, pixels written, missed frames, free memory.

## Not done (be aware)
- **Dirty-rectangle compositor**: the scene (wallpaper, windows, dock, menus) is still redrawn each frame; only the
  presentation step is differential. Per-window surfaces already exist. A damage-tracking compositor is the next step.
- **EDID / VBE-DDC / VBE modes**: need real-mode BIOS calls, impossible from this 32-bit protected-mode kernel with
  GRUB. Not implemented; refresh is measured or unknown.
- **SIMD**: not enabled (kernel is built with -mno-sse and does not save FPU/SSE state on task switches).
- **Write-combining frame buffer (PAT)**: not enabled; the frame buffer is still mapped uncached.
- Terminal and Notepad now use myFont Mono (14 px, 8 x 16 cells; bright colours use Mono Bold). Characters outside printable ASCII still use the 8x16 bitmap font. The boot text console (before the desktop) is unchanged.
- Text scale above 4 (for example the 48 px welcome greeting) is capped at 32 px.
- No Devanagari glyphs in myFont yet; Hindi is written in Latin letters.
- Font licence is not chosen yet (see docs/fonts/myFont-README.md).
