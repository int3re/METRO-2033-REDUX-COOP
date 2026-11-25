"""ride.py [--save name | --level level] [--times "8,20,32,44,56"] [--seconds N] [--show regex]

The handcar ride of l03_chase with a joined player (default: the level from its start; the joiner comes while
the handcar is already going). Both games write their world ("!world n") and take a picture at each of --times -
seconds after the joiner's body arrived (both games see that at the same moment, whatever each one's clock and
however long the level took to load).

Checks:
  1  both games ended by themselves
  2  no crash
  3  the joined game's handcar is where the host's is (within 2.5 m at every time point: the handcar does 10 m a second and
     the two copies' clocks agree to a quarter of a second at best)
  4  on its own PC the joiner's body is beside the host's (within 1.6 m, not more than 0.6 m lower)
  5  on the host's PC the joiner's body is beside the host's too (within 3 m: the joined game puts its body beside
     the host as it sees the host, and that place goes back over the network - a quarter of a second in all when
     the test copies run slowly, and the handcar does 10 m a second)
  6  the two bodies are not in one spot (more than 0.3 m apart on both PCs)

Pictures of both sides go to re/shots/ride/.
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


level = opt("--level", r"2033\l03_chase")
save = opt("--save")
times = [float(x) for x in opt("--times", "8,20,32,44,56").split(",")]
seconds = int(opt("--seconds", str(int(max(times)) + 85)))  # (up to 65 s to the joiner's body, then the samples)
show = opt("--show")
os.environ["METRONET_NET"] = (os.environ.get("METRONET_NET", "") + ";ride = 1").strip(";")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
for c in (host, client):
    for f in os.listdir(c.dir) if os.path.isdir(c.dir) else []:
        if re.match(r"world-\d+\.txt$", f):
            os.remove(os.path.join(c.dir, f))
if save:
    dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_" + save), os.path.join(dst_dir, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"
else:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level

# (each sample takes 0.2 s itself, so the wait before the next one is that much shorter)
gaps = [times[0]] + [b - a - 0.2 for a, b in zip(times, times[1:])]
steps = lambda side: "".join(" | +%g: !world %d | +0.1: !net | +0.1: !shot %s%03d" % (g, i, side, t)
                             for i, (g, t) in enumerate(zip(gaps, times)))
ph = host.start(seconds, begin + " | ?pid== " + steps("h"), extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== " + steps("c"), bind_port=1001, autostart=False, after=(host, ph),
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

out_dir = os.path.join(REPO, "re", "shots", "ride")
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
for c, side in ((host, "host"), (client, "client")):
    files = shots(c, out_dir, 1.0, 0.6)
    if files:
        print("%s: %d pictures, sheet %s" % (side, len(files), sheet(files, os.path.join(out_dir, "sheet_%s.png" % side))))


def read(copy, n):
    out = {}
    p = os.path.join(copy.dir, "world-%d.txt" % n)
    if not os.path.exists(p):
        return None
    for l in open(p, encoding="latin1"):
        f = l.rstrip("\n").split("\t")
        if len(f) >= 11:
            out[int(f[0], 16)] = dict(cls=f[1], pos=(float(f[4]), float(f[5]), float(f[6])), parent=int(f[9], 16), name=f[10])
    return out


def dist(p, q):
    return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5


RAILS = ("A1BFC0", "B2BF10", "B2D110", "B2E100")
STATE = re.compile(r"net \[lab\]: network game \d+, host \w+, local slot (-?\d+), players \d+, level \w+, body ([0-9A-F]+)")
SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/")


def bodies(copy):
    """the ids of the host's body and the joiner's body, from the copy's last "!net" line"""
    for l in reversed(copy.log_lines()):
        if STATE.search(l):
            s = dict((int(k), int(v, 16)) for k, v in SLOT.findall(l))
            return s.get(0), s.get(1)
    return None, None


hb, jb = bodies(host)
rows, car_gap, own_gap, seen_gap, low = [], [], [], [], []
for n, t in enumerate(times):
    h, c = read(host, n), read(client, n)
    if h is None or c is None or hb not in h or jb not in h or hb not in c or jb not in c:
        rows.append("%5g s: no data (host %s, joiner %s)" % (t, "ok" if h else "none", "ok" if c else "none"))
        car_gap.append(None)
        continue
    cars = [i for i in h if h[i]["cls"] in RAILS and i in c and h[i]["parent"] == 0xFFFF and i == h[hb]["parent"]]
    gap = max([dist(h[i]["pos"], c[i]["pos"]) for i in cars] or [None]) if cars else None
    car_gap.append(gap)
    own_gap.append(dist(c[hb]["pos"], c[jb]["pos"]))
    seen_gap.append(dist(h[hb]["pos"], h[jb]["pos"]))
    low.append(max(c[hb]["pos"][1] - c[jb]["pos"][1], h[hb]["pos"][1] - h[jb]["pos"][1]))
    rows.append("%5g s: handcar apart %s m; bodies apart: joiner's PC %.2f m, host's PC %.2f m; joiner lower by %.2f m; host at (%.1f,%.1f,%.1f)"
                % (t, "%.2f" % gap if gap is not None else "-", own_gap[-1], seen_gap[-1], low[-1], *h[hb]["pos"]))

has = lambda copy, text: any(text in l for l in copy.log_lines())
checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
known = [g for g in car_gap if g is not None]
checks.append(("the joined game's handcar is where the host's is", len(known) >= 2 and max(known) <= 2.5, str(["%.2f" % g for g in known])))
checks.append(("on its own PC the joiner's body is beside the host's", bool(own_gap) and max(own_gap) <= 1.6 and max(low) <= 0.6,
               str(["%.2f" % g for g in own_gap])))
checks.append(("on the host's PC the joiner's body is beside the host's", bool(seen_gap) and max(seen_gap) <= 3.0, str(["%.2f" % g for g in seen_gap])))
checks.append(("the two bodies are not in one spot", bool(own_gap) and min(own_gap + seen_gap) > 0.3, ""))

for r in rows:
    print("   " + r)
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail[:300] + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|did not quit|pid==|rides|rail vehicle|link:|sits on|is off|free again")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l) and "cont_rail" not in l][-22:]:
            print(l[1:13] + l[19:250])
print("pictures: %s" % out_dir)
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
