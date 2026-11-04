"""Make test pictures with Pillow, decode them with src/gfx/image.c, compare pixels."""
import subprocess, sys, os, random
from PIL import Image, ImageDraw
here = os.path.dirname(os.path.abspath(__file__))
tmp = "/tmp/myos_img_test"; os.makedirs(tmp, exist_ok=True)
exe = tmp + "/image_test"
subprocess.check_call(["cc", "-w", "-std=gnu11", "-I" + here + "/../../include", here + "/image_test.c",
                       here + "/../../src/gfx/image.c", "-o", exe])
random.seed(1)
im = Image.new("RGB", (97, 61))
d = ImageDraw.Draw(im)
for i in range(40):
    x0, y0 = random.randint(0, 80), random.randint(0, 45)
    d.rectangle([x0, y0, x0 + random.randint(1, 16), y0 + random.randint(1, 15)],
                fill=tuple(random.randint(0, 255) for _ in range(3)))
rgba = im.convert("RGBA"); rgba.putalpha(180)
cases = [("a.png", im, {}), ("b.png", rgba, {}), ("c.bmp", im, {}), ("d.jpg", im, {"quality": 95}),
         ("e.jpg", im, {"quality": 90, "subsampling": 0}), ("f.png", im.convert("P"), {}), ("g.png", im.convert("L"), {}),
         ("h.jpg", im.convert("L"), {})]
bad = 0
for name, img, kw in cases:
    p = f"{tmp}/{name}"; img.save(p, **kw)
    out = subprocess.run([exe, p], capture_output=True, text=True).stdout.split("\n")
    if out[0].startswith("ERR"):
        print("FAIL", name, out[0]); bad += 1; continue
    w, h = map(int, out[0].split())
    ref = Image.open(p).convert("RGBA")
    worst = 0
    for line in out[1:7]:
        x, y, r, g, b = map(int, line.split())
        R, G, B, A = ref.getpixel((x, y))
        R, G, B = [(c * A + 255 * (255 - A)) // 255 for c in (R, G, B)]
        worst = max(worst, abs(R - r), abs(G - g), abs(B - b))
    ok = (w, h) == img.size and worst <= (24 if name.endswith("jpg") else 2)
    print("ok  " if ok else "FAIL", name, (w, h), "max diff", worst)
    bad += not ok
sys.exit(1 if bad else 0)
