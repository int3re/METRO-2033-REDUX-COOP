"""monster.py [--level level] [--class hex] [--who guest|host] [--seconds N] [--show regex]

A player's bullets against a live monster. On l19_defence (the nosalises of the level sleep a floor above) the
host brings the nearest one 5 m in front of the players and wakes it ("!bring"); then one player's pad turns to
it and fires bursts ("!hunt") for 25 s.

  --who guest (default): the joined player shoots. Its hits travel to the host as hit events.
  --who host:            the host shoots (the single-player path, for comparison).

Checks:
  1  both games ended by themselves
  2  no crash
  3  the monster was brought and both PCs have it near the players
  4  the shooter looked at it (a third of the steps on target)
  5  the monster's health fell on the host's PC (or it died)
  6  ... and on the joined PC

Pictures of both sides go to re/shots/monster/.
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


level = opt("--level", r"2033\l19_defence")
cls = opt("--class", "A2C650")
who = opt("--who", "guest")
seconds = int(opt("--seconds", "115"))
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
hunt = " | +0.3: !hunt %s 5000 255 60 14" % cls
near = " | +0.2: !near E | +0.1: !net"
hunts = lambda side: "".join(hunt + near + " | +0.1: !shot %s_hunt%d" % (side, k) for k in range(5))
waits = lambda side: "".join(" | +5: !near E | +0.1: !net | +0.1: !shot %s_wait%d" % (side, k) for k in range(5))
host_steps = " | +5: !bring %s 5 | +1: !near E | +0.1: !net | +0.1: !shot h_start" % cls + (hunts("h") if who == "host" else waits("h"))
client_steps = " | +6.5: !near E | +0.1: !net | +0.1: !shot c_start" + (hunts("c") if who == "guest" else waits("c"))
ph = host.start(seconds, begin + " | ?pid== " + host_steps + " | +1: !net", extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== " + client_steps + " | +1: !net", bind_port=1001, autostart=False, after=(host, ph),
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

out_dir = os.path.join(REPO, "re", "shots", "monster")
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
for c, side in ((host, "host"), (client, "client")):
    files = shots(c, out_dir, 1.0, 0.6)
    if files:
        print("%s: %d pictures, sheet %s" % (side, len(files), sheet(files, os.path.join(out_dir, "sheet_%s.png" % side))))

has = lambda copy, text: any(text in l for l in copy.log_lines())
brought = next((m.group(1) for l in host.log_lines() for m in [re.search(r"net: brought ([0-9A-F]+) ", l)] if m), None)


def healths(copy):
    out = []
    rx = re.compile(r"near\s+%s class \+%s health (-?[\d.]+) at ([\d.]+) m" % (brought, cls)) if brought else None
    for l in copy.log_lines():
        m = rx.search(l) if rx else None
        if m:
            out.append(float(m.group(1)))
    return out


hh, ch = healths(host), healths(client)
shooter = client if who == "guest" else host
on = steps = 0
for l in shooter.log_lines():
    m = re.search(r"lab: hunted class \+\w+ for \d+ ms: \d+ targets, on target (\d+) of (\d+) steps", l)
    if m:
        on += int(m.group(1))
        steps += int(m.group(2))
gone_h = bool(brought) and len(hh) >= 1 and len(hh) < 6   # it left the list: dead
fell = lambda v: len(v) >= 2 and (min(v[1:]) < v[0] - 0.05 or min(v) <= 0)

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the monster was brought and both PCs have it near", bool(brought) and bool(hh) and bool(ch), "id %s, host %d samples, joiner %d" % (brought, len(hh), len(ch))))
killed = len(hh) >= 2 and min(hh) <= 0
checks.append(("the shooter looked at it", steps > 0 and (on * 3 >= steps or (killed and on > 0)), "%d of %d steps%s" % (on, steps, ", then it was dead" if killed else "")))
checks.append(("the monster's health fell on the host's PC", fell(hh) or gone_h, str(hh)))
checks.append(("the monster's health fell on the joined PC", fell(ch) or (bool(ch) and len(ch) < 6), str(ch)))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|did not quit|pid==|lab: hunt|brought|nothing of class|near .*class \+%s" % cls)
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-26:]:
            print(l[1:13] + l[19:230])
print("pictures: %s" % out_dir)
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
