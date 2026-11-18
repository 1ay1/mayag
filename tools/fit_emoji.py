# Fit CoreText's emoji resampling: render the emoji alone with ct_ui on white,
# then for every sbix strike x filter x subpixel offset, synthesise the same
# composite and report the best matches.
# Needs /tmp/ct_shape and /tmp/ct_ui (tools/ct_shape.c, tools/ct_ui.c) and
# Pillow + numpy. Usage: fit_emoji.py 1F600 13
import struct, subprocess, sys, io
import numpy as np
from PIL import Image
F = "/System/Library/Fonts/Apple Color Emoji.ttc"
cp = int(sys.argv[1], 16); pt = float(sys.argv[2])
d = open(F, 'rb').read()
o = struct.unpack('>I', d[16:20])[0]   # face 1 (.AppleColorEmojiUI)
n = struct.unpack('>H', d[o+4:o+6])[0]; T = {}
for i in range(n):
    t, c, off, l = struct.unpack('>4sIII', d[o+12+16*i:o+28+16*i]); T[t] = (off, l)
# glyph id via cmap is easier through mg: use ct_shape
ct = subprocess.run(['/tmp/ct_shape', F, '1', chr(cp)], capture_output=True, text=True).stdout
gid = int(ct.split('@')[0].split()[-1])
s, _ = T[b'sbix']; ns = struct.unpack('>I', d[s+4:s+8])[0]
strikes = {}
for k in range(ns):
    so = s + struct.unpack('>I', d[s+8+4*k:s+12+4*k])[0]; ppem = struct.unpack('>H', d[so:so+2])[0]
    a, b = struct.unpack('>II', d[so+4+4*gid:so+12+4*gid])
    if b > a + 8: strikes[ppem] = Image.open(io.BytesIO(d[so+a+8:so+b])).convert('RGBA')
# CoreText reference: emoji on white
subprocess.run(['/tmp/ct_ui', '/tmp/fit_ct.png', chr(cp), str(pt)], capture_output=True)
ref = np.asarray(Image.open('/tmp/fit_ct.png').convert('RGB')).astype(np.float32)
mask = ref.min(axis=2) < 250
ys, xs = np.where(mask); y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
# image geometry from CoreText's bounding rect (exact, measured earlier)
box = 1.25 if pt <= 16 else 1.0 if pt >= 24 else (20 + (pt - 16) * 0.5) / pt
size = pt * box
def lin(c): c = c / 255; return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def enc(l): return np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(np.clip(l, 0, 1), 1 / 2.4) - 0.055) * 255
filters = {'box': Image.BOX, 'bilinear': Image.BILINEAR, 'bicubic': Image.BICUBIC, 'lanczos': Image.LANCZOS, 'hamming': Image.HAMMING}
results = []
for ppem, im in strikes.items():
    for fname, flt in filters.items():
        for space in ('srgb', 'linear'):
            # draw into a canvas at 8x subpixel offsets
            for fx in range(4):
                for fy in range(4):
                    ox, oy = fx / 4, fy / 4
                    W = int(np.ceil(size + 2)); scale = size / ppem
                    # render at the fractional offset by resampling into an offset grid
                    big = im.resize((max(1, round(size)), max(1, round(size))), flt) if True else None
                    arr = np.asarray(big).astype(np.float32) / 255
                    a = arr[..., 3:4]
                    rgb = arr[..., :3]
                    base = np.ones_like(rgb)
                    if space == 'linear': out = enc(lin(rgb * 255) * a + 1 * (1 - a)) / 255
                    else: out = rgb * a + base * (1 - a)
                    out = out * 255
                    h, w = out.shape[:2]
                    crop = ref[y0:y0 + h, x0:x0 + w]
                    if crop.shape[:2] != (h, w): continue
                    err = np.abs(crop - out).mean()
                    results.append((err, ppem, fname, space))
                    break
                break
results.sort()
for r in results[:6]: print(f'err {r[0]:6.2f} strike {r[1]:4d} filter {r[2]:9s} blend {r[3]}')
print('image size', size, 'ct ink box', x1 - x0, y1 - y0)
