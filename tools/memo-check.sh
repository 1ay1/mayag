#!/bin/sh
# Does memo() change what an example draws? Runs its --headless script with
# memo on and off, recording every frame, and compares them.
#   tools/memo-check.sh EXAMPLE
# A reused subtree that moved can land 1 ulp off a fresh layout, so a
# difference of 1/255 on a few edge pixels is expected; anything larger is not.
set -u
e=$1
here=$(cd "$(dirname "$0")/.." && pwd)
d=${XDG_CACHE_HOME:-$HOME/.cache}/mayag-memo-check
rm -rf "$d"; mkdir -p "$d/on" "$d/off"
"$here/build/examples/mayag_$e" --headless --record "$d/on" >/dev/null 2>&1
MAYAG_MEMO=0 "$here/build/examples/mayag_$e" --headless --record "$d/off" >/dev/null 2>&1
python3 - "$d" "$e" <<'PY'
import sys, glob, os
from PIL import Image, ImageChops
d, e = sys.argv[1], sys.argv[2]
n = differ = 0; worst = 0
for f in sorted(glob.glob(f"{d}/off/*.png")):
    n += 1
    b = f.replace("/off/", "/on/")
    if not os.path.exists(b): differ += 1; worst = 255; continue
    if open(f, "rb").read() == open(b, "rb").read(): continue
    diff = ImageChops.difference(Image.open(f).convert("RGBA"), Image.open(b).convert("RGBA"))
    m = max(hi for lo, hi in diff.getextrema())
    if m: differ += 1; worst = max(worst, m)
print(f"{e}: {n} frames, {differ} differ, max delta {worst}/255")
sys.exit(0 if worst <= 1 else 1)
PY
rc=$?
rm -rf "$d"
exit $rc
