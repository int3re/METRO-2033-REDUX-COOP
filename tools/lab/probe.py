"""probe.py [--copy test-m33c] [--seconds N] [--save hunter_start | --level level] [--show regex] "steps"

One test copy (mode = host, nobody joins, the lab's port 1003 so that it can run beside a two-copy test) from the
lab's save (or a level's start); once the level runs the given steps are done (" | " between steps, as in [lab]
script). Prints the log lines that match --show (default: the lab's and the mod's own lines after the steps begin).

  python tools/lab/probe.py "+2: !actions 20 | +1: !pad 4000 0 0 200"
"""
import os, re, shutil, sys
from lab import Copy, wait, require_memory, shots, sheet, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


name = opt("--copy", "test-m33c")
seconds = int(opt("--seconds", "90"))
save = opt("--save", "hunter_start")
level = opt("--level")
show = opt("--show", r"lab: (?!waiting|got|pad buttons|peak)|net: |hud:|CRASH|stack:")
mode = opt("--mode", "host")
out = opt("--shots")  # a folder name under re/shots for the copy's pictures ("!shot name" in the steps)
steps = a[0] if a else ""

require_memory(1)
c = Copy(name)
if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?loader_finish: mapstate_running" % level
else:
    dst = os.path.join(c.user, "4A Games", "Metro 2033")
    os.makedirs(dst, exist_ok=True)
    shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_" + save), os.path.join(dst, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start | ?loader_finish: mapstate_running"
p = c.start(seconds, begin + " | +3: !mark steps" + (" | " + steps if steps else ""), bind_port=1003, extra="[net]\nmode = %s" % mode)
code = wait([p], seconds + 180)[0]
if out:
    files = shots(c, os.path.join(REPO, "re", "shots", out))
    if files:
        print("%d pictures, sheet %s" % (len(files), sheet(files, os.path.join(REPO, "re", "shots", out, "sheet.png"), columns=3, width=480)))
lines = c.log_lines()
print("ended: %s; crash: %s" % (code, any("CRASH " in l for l in lines)))
rx = re.compile(show)
started = False
for l in lines:
    if "lab: mark steps" in l:
        started = True
    if started and rx.search(l):
        print("  " + l[1:13] + l[19:230])
