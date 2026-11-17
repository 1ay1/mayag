#!/usr/bin/env python3
"""Compare toolkits: time to first frame, CPU while animating, memory.

  bench/compare.py [runs]

Time to first frame: process spawn -> the first wl_surface.commit that puts
pixels up (after a wl_surface.attach, or the commit a Vulkan present makes),
read from WAYLAND_DEBUG timestamps, which every libwayland client prints the
same way. CPU and RSS are read from /proc while the app animates.
"""
import os, re, subprocess, sys, time, statistics

APPS = {
    "mayag": ["/tmp/bench/mayag_app"],
    "gtk4": ["/tmp/bench/gtk4_app"],
    "qt6": ["/tmp/bench/qt_app"],
}
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
CLK = os.sysconf("SC_CLK_TCK")
stamp = re.compile(r"^\[(\d+):(\d+):(\d+)\.(\d+)\]")


def first_frame_ms(cmd):
    """Spawn -> first wl_surface.commit after an attach of a real buffer.

    libwayland stamps each request with local wall-clock time, so spawn is
    taken on the same clock."""
    env = dict(os.environ, WAYLAND_DEBUG="client")
    import datetime
    # libwayland stamps requests with UTC wall-clock time of day.
    now = datetime.datetime.now(datetime.timezone.utc)
    midnight = now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
    t0 = time.time()
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    attached = False
    commit = None
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = p.stderr.readline()
        if not line:
            break
        m = stamp.match(line)
        if not m or "wl_surface" not in line:
            continue
        if ".attach(wl_buffer" in line:
            attached = True
        elif ".commit()" in line and attached:
            h, mi, s, us = (int(x) for x in m.groups())
            commit = midnight + h * 3600 + mi * 60 + s + us / 1e6
            break
    p.terminate()
    try:
        p.wait(timeout=2)
    except subprocess.TimeoutExpired:
        p.kill()
    return None if commit is None else (commit - t0) * 1000.0


def active_cpu(cmd, secs=4.0):
    p = subprocess.Popen(cmd + ["animate"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    def ticks():
        return sum(int(x) for x in open(f"/proc/{p.pid}/stat").read().rsplit(")", 1)[1].split()[11:13])

    a = ticks()
    time.sleep(secs)
    b = ticks()
    rss = 0
    for l in open(f"/proc/{p.pid}/status"):
        if l.startswith("VmRSS"):
            rss = int(l.split()[1]) // 1024
    threads = len(os.listdir(f"/proc/{p.pid}/task"))
    p.terminate()
    try:
        p.wait(timeout=2)
    except subprocess.TimeoutExpired:
        p.kill()
    return 100.0 * (b - a) / CLK / secs, rss, threads


print(f"{'':8} {'first frame (ms, median of %d)' % RUNS:>32}  {'cpu animating':>14}  {'rss':>7}  threads")
for name, cmd in APPS.items():
    ffs = [x for x in (first_frame_ms(cmd) for _ in range(RUNS)) if x is not None]
    cpu, rss, thr = active_cpu(cmd)
    ff = f"{statistics.median(ffs):.0f}  [{min(ffs):.0f}-{max(ffs):.0f}]" if ffs else "n/a"
    print(f"{name:8} {ff:>32}  {cpu:12.1f} %  {rss:5d} MB  {thr}")
