# Pictures and the camera

## Decoders (`include/image.h`, `src/gfx/image.c`)

```c
const char *err;
struct gfx_surface *pic = image_load("/Users/arsh/Pictures/cat.jpg", GFX_WHITE, &err);
```

| Format | Supported |
|--------|-----------|
| BMP | 8, 24 and 32 bit, uncompressed |
| PNG | non-interlaced; 8 bit colour (grey, RGB, RGBA, grey+alpha) and palette/grey up to 8 bit |
| JPEG | baseline and extended sequential Huffman, grey or YCbCr, any common sampling, restart markers |

Not supported: progressive JPEG, interlaced PNG, 16 bit PNG, GIF, WebP. Pictures larger than
about 1400 x 1000 pixels are shrunk while loading so they fit in the 32 MB kernel heap.
`image_fit()` scales to a box, `image_save_bmp()` writes a BMP, `image_is_picture()` checks the extension.
`tests/host/image_check.py` decodes Pillow-made pictures with the kernel code and compares pixels.

## Apps

- **Photos**: thumbnails of `~/Pictures`, click for the full picture, Left/Right to browse,
  File > Set as Wallpaper. Double-click a picture in Files to open it here.
- **Camera**: live preview and Take Photo (saved as `~/Pictures/Photo N.bmp`).

## Camera drivers (`include/camera.h`)

A driver fills `struct camera` (name, size, `grab()`) and calls `camera_register()`. MyOS includes
one built-in source, an animated test pattern, so the app works in every emulator. **Real USB webcams
are not supported yet**: UVC cameras stream over isochronous USB transfers, which the USB stack does
not implement. That is the next step if you want a real camera.

## Dock icons

Put `AppName.png` (or .bmp/.jpg) in `/Users/<you>/Library/Icons/` (or `/System/Icons/`) and press
Settings > Dock > Reload icons. Settings > Dock also switches coloured tiles and the icon size.
