#!/bin/sh
# Measure input-to-photon on a real macOS window: launch an example, drive the
# pointer with HID-level CGEvents (tools/hid_mover.c), and print the window's
# own measurement (presented-handler time minus NSEvent hardware timestamp).
#   tools/photon-bench.sh [example] [seconds]
# Env passes through: MAYAG_PRESENT=fifo for the queued baseline,
# MAYAG_PHOTON=2 for a per-frame trace in /tmp/photon_bench.log.
# The terminal needs Accessibility permission to post events.
ex=${1:-reactive}; secs=${2:-3}
here=$(cd "$(dirname "$0")/.." && pwd)
bin="$here/build/examples/mayag_$ex"
mover="$here/build/hid_mover"
if [ ! -x "$mover" ] || [ "$here/tools/hid_mover.c" -nt "$mover" ]; then
    cc -O2 -o "$mover" "$here/tools/hid_mover.c" -framework ApplicationServices || exit 1
fi
MAYAG_PHOTON=${MAYAG_PHOTON:-1} "$bin" >/tmp/photon_bench.log 2>&1 &
pid=$!
sleep 1.5
"$mover" "$secs"
kill -TERM "$pid"
wait "$pid" 2>/dev/null
grep "input->photon" /tmp/photon_bench.log
