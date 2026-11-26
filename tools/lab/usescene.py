"""usescene.py [--level level] [--target id] [--seconds N] [--show regex]

Another player's "use" with a scene, on the host: the prologue's wheel (l00_intro, entity 6E00: the user's game on
2026-11-26 - "used a thing, waits on 6E00"). The joined copy uses it ("!useid"); its own PC plays the scene. The
host plays that use too so that its story goes on, but the blocks of the scene are only finished there - the host's
player must not move (the camera taken to the wheel, the hands, the body hung on the wheel: 0.4.2 pulled both).

Checks:
  1  both games ended by themselves
  2  no crash
  3  the joined game used the thing and told the host
  4  the host played that use
  5  the host's own story went on (the thing's motion started there by the host's script)
  6  the joiner's own use was not turned back by the host's word
  7  the host's player stayed where it stood (less than 0.3 m) while the joiner's scene ran
"""
import os, re, sys
from lab import Copy, wait, wait_hosting, require_memory, shots, sheet, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level", r"2033\l00_intro")
target = opt("--target", "6E00")
seconds = int(opt("--seconds", "120"))
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
# (both PCs turn the wheel's use block on: at the start of the level the script has not come to it yet)
ph = host.start(seconds, begin + " | ?pid== | +1: !useon %s | +0.5: !motions 40 | +4.5: !mark before | +0.1: !net | +4: !shot h_during | +10: !mark after | +0.1: !net" % target,
                extra="[net]\nmode = host")
late = wait_hosting(host, ph)
pc = client.start(max(60, int(round(seconds - late))), "0.3: >stop_video | 8: !finish | ?pid== | +0.5: !motions 40 | +0.5: !useon %s | +3: !useid %s | +1.5: !shot j_during | +8: !net" % (target, target),
                  bind_port=1001, autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)
for c, side in ((host, "host"), (client, "joiner")):
    files = shots(c, os.path.join(REPO, "re", "shots", "usescene", side))
    if files:
        print("%s: %s" % (side, sheet(files, os.path.join(REPO, "re", "shots", "usescene", side, "sheet.png"), columns=2, width=480)))

STATE = re.compile(r"net \[lab\]: network game \d+, host \w+, local slot (-?\d+), players (\d+), level \w+, body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)")
places = []
for l in host.log_lines():
    m = STATE.search(l)
    if m and m.group(1) == "0":
        places.append(tuple(float(x) for x in m.group(4, 5, 6)))
moved = ((places[0][0] - places[-1][0]) ** 2 + (places[0][2] - places[-1][2]) ** 2) ** 0.5 if len(places) >= 2 else -1.0
has = lambda c, t: any(t in l for l in c.log_lines())
checks = [
    ("both games ended by themselves", codes == [0, 0], str(codes)),
    ("no crash", not any(has(c, "CRASH ") for c in (host, client)), ""),
    ("the joined game used the thing and told the host", has(client, "the lab uses %s" % target) and has(client, "a thing was used here"), ""),
    ("the host played that use", has(host, "used a thing") and has(host, "fired here too"), ""),
    ("the host's own story went on with that use (the thing's motion started by the host's script)",
     any(("lab: motion %s " % target) in l and "(for the host)" not in l for l in host.log_lines()), ""),
    ("the joiner's own use was not turned back by the host's word", not has(client, "plays 'ventel_idle' as on the host"), ""),
    ("the host's player stayed where it stood", 0 <= moved < 0.3, "%.2f m" % moved),
]
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"use|scene block|CRASH|stack:|would hang|net \[lab")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-25:]:
            print(l[1:13] + l[19:240])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
