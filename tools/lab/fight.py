"""fight.py [--seconds N] [--hunt-at T] [--show regex]

A joined player's bullets against a monster. The run starts from the save chase_fight (l03_chase, the handcar
after the vision): a nosalis jumps on the handcar about 30 s after the load and kills a host who does nothing.
A second copy joins and gets its seat beside the host ([net] ride = 1); from --hunt-at (default 46 s) its pad
turns to the nearest nosalis and fires bursts ("!hunt"). The host does nothing.

Checks:
  1  both games ended by themselves
  2  no crash
  3  the joiner had its body when the nosalis came
  4  the nosalis was hurt on the host's PC (its health fell there, or it is gone / dead)
  5  the host lived (its health never reached zero)

Pictures of both sides go to re/shots/fight/.
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


seconds = int(opt("--seconds", "125"))
hunt_at = float(opt("--hunt-at", "0"))  # 0: a second after the body came
show = opt("--show")
os.environ["METRONET_NET"] = (os.environ.get("METRONET_NET", "") + ";ride = 1").strip(";")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
os.makedirs(dst_dir, exist_ok=True)
shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_chase_fight"), os.path.join(dst_dir, "2033_lab_start"))

times = [t for t in (42, 46, 50, 54, 58, 62, 66, 70, 76, 84, 92, 100) if t < seconds - 6]
host_steps = "".join(" | %g: !near C | +0.1: !net | +0.1: !shot h%03d" % (t, t) for t in times)
height = opt("--height", "150")
# (the hunt holds the script: five goes of six seconds with a picture after each)
client_steps = " | %s: !mark hunt | +0.1: !near C" % (("%g" % hunt_at) if hunt_at else "+1") + "".join(
    " | +0.1: !hunt A2C650 6000 255 %s 12 attacker | +0.1: !shot c_hunt%d" % (height, k) for k in range(5)) + "".join(
    " | %g: !net | +0.1: !shot c%03d" % (t, t) for t in times if t > (hunt_at or 40) + 33)
ph = host.start(seconds, "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start | ?pid== " + host_steps + " | +1: !net", extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== | +0.5: !shot c000" + client_steps + " | +1: !net", bind_port=1001,
                  autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

out_dir = os.path.join(REPO, "re", "shots", "fight")
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
for c, side in ((host, "host"), (client, "client")):
    files = shots(c, out_dir, 1.0, 0.6)
    if files:
        print("%s: %d pictures, sheet %s" % (side, len(files), sheet(files, os.path.join(out_dir, "sheet_%s.png" % side))))

NEAR = re.compile(r"near\s+([0-9A-F]+) class \+A2C650 health (-?[\d.]+) at ([\d.]+) m .* '([^']*)'")
SLOT0 = re.compile(r" s0=[0-9A-F]+/\d/(?:peer|-)\([-\d.,]+\)h(-?[\d.]+)")
has = lambda copy, text: any(text in l for l in copy.log_lines())

monsters = {}  # id -> [health at each sample it was near]
host_health = []
for l in host.log_lines():
    m = NEAR.search(l)
    if m:
        monsters.setdefault(m.group(1) + " " + m.group(4), []).append(float(m.group(2)))
    m = SLOT0.search(l)
    if m and "net [lab]" in l:
        host_health.append(float(m.group(1)))
hunt = "; ".join(l[25:140] for l in client.log_lines() if "lab: hunted" in l) or "no hunt line"
bodies = [m.group(1) for l in client.log_lines() for m in [re.search(r"pid==1, eid==(\d+)", l)] if m]
hurt = [k for k, v in monsters.items() if min(v) < v[0] - 0.05 or min(v) <= 0]

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the joiner had its body", len(bodies) >= 1, str(bodies)))
checks.append(("the nosalis was hurt on the host's PC", bool(hurt), "; ".join("%s: %s" % (k, v) for k, v in monsters.items())[:400]))
checks.append(("the host lived", bool(host_health) and min(host_health) > 0, str(host_health)))

print("   " + hunt)
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|did not quit|pid==|rides|rail vehicle|link:|sits on|lab: hunt|lab: mark|a hit of")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l) and "cont_rail" not in l][-22:]:
            print(l[1:13] + l[19:250])
print("pictures: %s" % out_dir)
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
