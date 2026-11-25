"""menu.py [--out name] [--seconds N] [--copy test-m33] [--extra "ini lines"] [--show regex] steps

One test copy in the game's main menu: the copy starts, skips the opening movie, and once the menu runs the
given script steps are done (" | " between steps, as in [lab] script: "!shot name", "!pad buttons lx ly ms",
console commands ...). The pictures go to re/shots/<out>/ with a sheet; --show prints the log lines that match.

  python tools/lab/menu.py --out menu1 "+3: !shot menu"
"""
import os, sys
from lab import Copy, wait, require_memory, shots, sheet, show, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


out = opt("--out", "menu")
seconds = int(opt("--seconds", "50"))
name = opt("--copy", "test-m33")
extra = opt("--extra", "").replace("\\n", "\n")
grep = opt("--show")
steps = a[0] if a else "+3: !shot menu"

require_memory(1)
c = Copy(name)
p = c.start(seconds, "0.3: >stop_video | ?loader: mapstate_loaded_and_waiting | +3: !finish | ?loader_finish: mapstate_running | " + steps, autostart=False, extra=extra)
codes = wait([p], seconds + 120)
out_dir = os.path.join(REPO, "re", "shots", out)
files = shots(c, out_dir)
if files:
    print("%d pictures, sheet %s" % (len(files), sheet(files, os.path.join(out_dir, "sheet.png"), columns=3, width=480)))
if grep:
    show(c, grep=grep)
crashed = any("CRASH " in l for l in c.log_lines())
print("ended: %s; crash: %s" % (codes, "YES" if crashed else "no"))
