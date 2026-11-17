# bench — mayag against other toolkits

The same app in each toolkit: a title, a progress bar, and a scrolling list of
200 two-line rows. `animate` moves the progress bar every frame.

    build the three apps (below), then:
    python3 bench/compare.py [runs]

Build lines (Arch, GTK 4.24, Qt 6.12):

    c++ -O2 bench/gtk4_app.cpp -o /tmp/bench/gtk4_app $(pkg-config --cflags --libs gtk4)
    c++ -O2 -fPIC bench/qt_app.cpp -o /tmp/bench/qt_app $(pkg-config --cflags --libs Qt6Widgets)
    # mayag: the flags from build/compile_commands.json for any example, plus
    #   build/third_party/jaal/libjaal.a -ldl

What it measures:

- first frame: process spawn to the first `wl_surface.commit` after a buffer
  is attached, from `WAYLAND_DEBUG` timestamps (the same libwayland logging in
  every toolkit, so the cut point is identical).
- cpu animating: share of one core over 4 s while the bar animates.
- rss: resident memory while animating. Qt Widgets renders on the CPU and
  never loads the GPU driver, so its RSS doesn't include ~60 MB of NVIDIA
  driver that GTK4 and mayag both carry.
