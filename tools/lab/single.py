"""single.py [--copy name] [--level level] [--seconds N]

The mod with `mode = off`: plain single player must stay what it was. One copy starts a level with "map", walks,
fires, saves ("gamesave lab_single"), loads that save, walks again and quits. (With the mod off nothing takes the
loader's pause off as a key press does in real play: the lab does it, "!unpause".)

Checks:
  1  the game ended by itself
  2  no crash
  3  it never became a network game
  4  the save was written and loaded
  5  the player walked, and the load put it back where it was saved
"""
import re, sys
from lab import Copy, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


copy_name = opt("--copy", "test-m33c")
level = opt("--level", r"2033\l19_defence")
seconds = int(opt("--seconds", "100"))

require_memory(1)
c = Copy(copy_name)
# (every step waits for the game's own line, not for the clock: a level loads 10 - 30 s as the PC happens to be busy)
script = (r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?^map 2033 | ?loader_finish: mapstate_running | +2: !unpause | +2: !net"
          r" | +0.5: !pad 0 0 32000 2500 0 0 0 255 | +3.5: !net"
          # (the game writes a save a few seconds after it is asked to: the load waits for the file's line)
          r" | +0.5: gamesave lab_single | ?content_open: 2033_lab_single | +2: gameload lab_single"
          r" | ?loader: mapstate_loaded_and_waiting | +1: !finish | +2: !unpause | +2: !net"
          r" | +0.5: !pad 0 0 32000 2500 | +3.5: !net") % level
code = c.run(seconds, script, bind_port=1003 if copy_name != "test-m33" else None, extra="[net]\nmode = off")

STATE = re.compile(r"net \[lab\]: network game (\d+), host (\w+), local slot (-?\d+), players (\d+), level (\w+), body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)")
st = [m for l in c.log_lines() for m in [STATE.search(l)] if m]
pos = [tuple(float(x) for x in m.group(7, 8, 9)) for m in st]
has = lambda text: any(text in l for l in c.log_lines())
moved = lambda p, q: sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5

checks = []
checks.append(("the game ended by itself", code == 0, str(code)))
checks.append(("no crash", not has("CRASH "), ""))
checks.append(("it never became a network game", bool(st) and all(m.group(1) == "0" for m in st), str([m.group(1) for m in st])))
checks.append(("the save was written and loaded", has("total : gamesave") and has("^gameload lab_single"), ""))
# (after a load in single player the lab's pad does not move the player: the game waits there for a real key -
# the check is that the load put the player where the save was made, not where the level starts)
checks.append(("the player walked, and the load put it back where it was saved", len(pos) >= 3 and moved(pos[0], pos[1]) > 1
               and moved(pos[1], pos[2]) < 0.5, str(["(%.1f,%.1f,%.1f)" % p for p in pos[:3]])))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed:
    rx = re.compile(r"CRASH|  r[a-z0-9]+ |stack:|session:|net \[lab|gamesave|gameload|did not quit")
    for l in [l for l in c.log_lines() if rx.search(l)][-20:]:
        print(l[1:13] + l[19:220])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
