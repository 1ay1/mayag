#!/usr/bin/env bash
# tools/test-remote.sh — exercise mayag over SSH, with no display attached.
#
# mayag is a windowing framework, but almost all of it can be tested without a
# compositor: the test suite is pure logic, --headless drives the real app
# without pixels, and --png renders a real frame to an image you can view
# inline (kitty) or copy back. This script runs that display-free flow in one
# command, and — if you ask for it — spins up a headless weston so the actual
# window backends run too.
#
#   tools/test-remote.sh            # configure, build, ctest, headless + png smoke
#   tools/test-remote.sh --quick    # skip the full ctest, just build + smoke
#   tools/test-remote.sh --window   # also run a real window under headless weston
#   tools/test-remote.sh --show     # after --png, display frames inline via kitty
#
# Safe to run over mosh/tmux: nothing here needs $WAYLAND_DISPLAY, and the
# window backends' live tests skip themselves when no compositor is present.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${MAYAG_BUILD:-$ROOT/build}"
QUICK=0 WINDOW=0 SHOW=0

for arg in "$@"; do
    case "$arg" in
        --quick)  QUICK=1 ;;
        --window) WINDOW=1 ;;
        --show)   SHOW=1 ;;
        -h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }

# ── configure + build ─────────────────────────────────────────────────────
say "configure + build"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j

# ── the test suite (pure logic; live-window halves self-skip) ─────────────
if [[ "$QUICK" -eq 0 ]]; then
    say "ctest (display-free; window tests skip without a compositor)"
    # Run without $WAYLAND_DISPLAY so the live-window checks skip cleanly even
    # if a compositor happens to be reachable — this is the pure-SSH result.
    ( cd "$BUILD" && env -u WAYLAND_DISPLAY ctest --output-on-failure ) || true
fi

# ── every example, headless: drives the real Program, asserts, exits ──────
say "headless smoke (the real app logic, no pixels)"
for exe in "$BUILD"/examples/mayag_*; do
    [[ -x "$exe" ]] || continue
    name="$(basename "$exe")"
    if timeout 20 "$exe" --headless >/dev/null 2>&1; then
        printf '  ok    %s --headless\n' "$name"
    else
        printf '  FAIL  %s --headless\n' "$name"
    fi
done

# ── every example, --png: a real frame to an image ────────────────────────
say "render frames to /tmp/mayag-frames/"
mkdir -p /tmp/mayag-frames
for exe in "$BUILD"/examples/mayag_*; do
    [[ -x "$exe" ]] || continue
    name="$(basename "$exe")"
    out="/tmp/mayag-frames/${name}.png"
    if timeout 20 "$exe" --png "$out" >/dev/null 2>&1 && [[ -s "$out" ]]; then
        printf '  ok    %s -> %s\n' "$name" "$out"
    else
        printf '  --    %s (no --png support)\n' "$name"
    fi
done

# ── optional: view frames inline over SSH (kitty) ─────────────────────────
if [[ "$SHOW" -eq 1 ]]; then
    say "inline preview (kitty graphics protocol)"
    if command -v kitten >/dev/null; then
        for png in /tmp/mayag-frames/*.png; do
            [[ -s "$png" ]] || continue
            printf '\n%s\n' "$(basename "$png")"
            kitten icat --align left "$png" || true
        done
    else
        echo "  kitten not found — scp /tmp/mayag-frames/*.png to view locally"
    fi
fi

# ── optional: a real window under a headless compositor ───────────────────
if [[ "$WINDOW" -eq 1 ]]; then
    say "real window under headless weston"
    if ! command -v weston >/dev/null; then
        echo "  weston not installed:  sudo pacman -S weston   (or apt install weston)"
    else
        export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/mayag-wl}"
        mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
        weston --backend=headless --socket=mayag-remote --idle-time=0 \
               >/tmp/mayag-weston.log 2>&1 &
        WESTON_PID=$!
        trap 'kill "$WESTON_PID" 2>/dev/null || true' EXIT
        for _ in $(seq 1 50); do
            [[ -S "$XDG_RUNTIME_DIR/mayag-remote" ]] && break
            sleep 0.2
        done
        export WAYLAND_DISPLAY=mayag-remote
        echo "  weston up on \$WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
        # The live-window tests now run for real against the virtual output.
        if [[ -x "$BUILD/mayag_wayland_tests" ]]; then
            "$BUILD/mayag_wayland_tests" || true
        fi
        echo "  (a windowed example would open against weston, e.g.:"
        echo "     WAYLAND_DISPLAY=$WAYLAND_DISPLAY $BUILD/examples/mayag_live )"
    fi
fi

say "done"
