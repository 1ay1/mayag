#!/bin/sh
# Measure input-to-photon on Linux/Hyprland with real kernel input: launch an
# example, warp the pointer into its window, wiggle a uinput mouse at a fixed
# rate, and print the window's own wp_presentation measurement.
#   tools/photon-bench-linux.sh [example] [seconds] [hz]
# Needs write access to /dev/uinput (the `input` group or an ACL).
set -u
ex=${1:-reactive}
secs=${2:-3}
hz=${3:-250}
here=$(cd "$(dirname "$0")/.." && pwd)
mover=/tmp/mayag_uinput_mover
[ -x "$mover" ] || cc -O2 "$here/tools/uinput_mover.c" -o "$mover"

log=/tmp/photon_$ex.log
MAYAG_PHOTON=${MAYAG_PHOTON:-1} MAYAG_FRAMESTATS=1 "$here/build/examples/mayag_$ex" >"$log" 2>&1 &
pid=$!
# Wait for the window to map, then put the pointer in its middle.
i=0
while [ $i -lt 50 ]; do
    geo=$(hyprctl clients -j | python3 -c "
import json,sys
for c in json.load(sys.stdin):
    if c.get('pid') == $pid:
        x,y=c['at']; w,h=c['size']; print(x+w//2, y+h//2); break")
    [ -n "$geo" ] && break
    i=$((i + 1)); sleep 0.05
done
[ -n "$geo" ] && hyprctl dispatch movecursor $geo >/dev/null || true
"$mover" "$secs" "$hz"
kill -TERM $pid 2>/dev/null || true   # background jobs start with SIGINT ignored
wait $pid 2>/dev/null || true
grep 'input->photon' "$log" || { echo "no photon samples (see $log)"; exit 1; }
