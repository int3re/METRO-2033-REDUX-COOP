"""world.py [--level level | --save name] [--times "45,75,105"] [--seconds N] [--host steps] [--client steps]
            [--eps metres] [--list N] [--solo-compare]

Are the two players in one world? A co-op run of two test copies; at each of --times (seconds from the start
of the games) both write every entity they hold ("!world n": id, class, flags, health, position, parent,
name), and the files are compared:

  - entities only one PC has (by id; and whether the other PC has one of the same name under another id)
  - entities both have, but in different places (more than --eps metres apart, default 0.5), actors first
  - actors whose health differs, entities hanging on different parents

--host / --client: extra lab steps for a side (run after the joiner's body arrived).
The result line counts the differences of the last time point; the exit code is 0 when the run itself went
well (both ended by themselves, no crash) - differences are the finding, not a failure.
"""
import os, re, shutil, sys
from lab import Copy, wait, require_memory, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level")
save = opt("--save", "hunter_start")
times = [float(x) for x in opt("--times", "45,75,105").split(",")]
seconds = int(opt("--seconds", str(int(max(times)) + 27)))
host_steps, client_steps = opt("--host", ""), opt("--client", "")
eps = float(opt("--eps", "0.5"))
shown = int(opt("--list", "25"))

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
for c in (host, client):
    for f in os.listdir(c.dir) if os.path.isdir(c.dir) else []:
        if re.match(r"world-\d+\.txt$", f):
            os.remove(os.path.join(c.dir, f))
if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    src = os.path.join(REPO, "re", "lab-saves", "2033_" + save)
    dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

dumps = "".join(" | %g: !world %d | +0.1: !net" % (t, i) for i, t in enumerate(times))
ph = host.start(seconds, begin + " | ?pid== " + ((" | " + host_steps) if host_steps else "") + dumps, extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== " + ((" | " + client_steps) if client_steps else "") + dumps,
                  bind_port=1001, autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)


def read(copy, n):
    out = {}
    p = os.path.join(copy.dir, "world-%d.txt" % n)
    if not os.path.exists(p):
        return None
    for l in open(p, encoding="latin1"):
        f = l.rstrip("\n").split("\t")
        if len(f) < 11:
            continue
        out[int(f[0], 16)] = dict(cls=f[1], flags=int(f[2], 16), health=None if f[3] == "-" else float(f[3]),
                                  pos=(float(f[4]), float(f[5]), float(f[6])), fwd=(float(f[7]), float(f[8])),
                                  parent=int(f[9], 16), name=f[10])
    return out


def dist(p, q):
    return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5


def line(i, e):
    return "%4X +%s f=%X %s(%.1f,%.1f,%.1f) p=%X '%s'" % (i, e["cls"], e["flags"], ("h%.2f " % e["health"]) if e["health"] is not None else "",
                                                       e["pos"][0], e["pos"][1], e["pos"][2], e["parent"], e["name"])


has = lambda copy, text: any(text in l for l in copy.log_lines())
total = None
for n, t in enumerate(times):
    h, c = read(host, n), read(client, n)
    print("==== %g s" % t)
    if h is None or c is None:
        print("   no dump (%s)" % ("host" if h is None else "joiner"))
        continue
    only_h = [i for i in h if i not in c]
    only_c = [i for i in c if i not in h]
    both = [i for i in h if i in c]
    names_c = {}
    for i, e in c.items():
        names_c.setdefault(e["name"], []).append(i)
    names_h = {}
    for i, e in h.items():
        names_h.setdefault(e["name"], []).append(i)
    moved = sorted(((dist(h[i]["pos"], c[i]["pos"]), i) for i in both if dist(h[i]["pos"], c[i]["pos"]) > eps), reverse=True)
    moved_actors = [(d, i) for d, i in moved if h[i]["health"] is not None]
    moved_roots = [(d, i) for d, i in moved if h[i]["health"] is None and h[i]["parent"] == 0xFFFF and c[i]["parent"] == 0xFFFF]
    health = [i for i in both if h[i]["health"] is not None and c[i]["health"] is not None and abs(h[i]["health"] - c[i]["health"]) > 0.02]
    parents = [i for i in both if h[i]["parent"] != c[i]["parent"]]
    flags = [i for i in both if (h[i]["flags"] ^ c[i]["flags"]) & ~0x3]
    print("   host %d entities, joiner %d, both %d; only host %d, only joiner %d" % (len(h), len(c), len(both), len(only_h), len(only_c)))
    print("   in different places: %d (actors %d, free-standing things %d); health differs: %d; parent differs: %d; flags differ: %d"
          % (len(moved), len(moved_actors), len(moved_roots), len(health), len(parents), len(flags)))
    total = (len(only_h), len(only_c), len(moved_actors), len(moved_roots), len(health), len(parents))
    for title, ids, src, names in (("only the host has", only_h, h, names_c), ("only the joiner has", only_c, c, names_h)):
        for i in ids[:shown]:
            twin = [x for x in names.get(src[i]["name"], []) if x != i]
            print("   %s: %s%s" % (title, line(i, src[i]), ("   (same name under %s there)" % ",".join("%X" % x for x in twin[:3])) if twin else ""))
        if len(ids) > shown:
            print("   ... and %d more" % (len(ids) - shown))
    for title, rows in (("actor apart", moved_actors), ("thing apart", moved_roots)):
        for d, i in rows[:shown]:
            print("   %s %.1f m: host %s | joiner (%.1f,%.1f,%.1f) f=%X p=%X%s" % (
                title, d, line(i, h[i]), c[i]["pos"][0], c[i]["pos"][1], c[i]["pos"][2], c[i]["flags"], c[i]["parent"],
                (" h%.2f" % c[i]["health"]) if c[i]["health"] is not None else ""))
        if len(rows) > shown:
            print("   ... and %d more" % (len(rows) - shown))
    for i in health[:shown]:
        print("   health: host %s | joiner h%.2f" % (line(i, h[i]), c[i]["health"]))
    for i in parents[:shown]:
        print("   parent: host %s | joiner p=%X (%.1f,%.1f,%.1f)" % (line(i, h[i]), c[i]["parent"], c[i]["pos"][0], c[i]["pos"][1], c[i]["pos"][2]))
    for i in flags[:min(shown, 12)]:
        print("   flags: host %s | joiner f=%X" % (line(i, h[i]), c[i]["flags"]))

crashes = [c.name for c in (host, client) if has(c, "CRASH ")]
ok = codes == [0, 0] and not crashes
print("run: codes %s%s" % (codes, (", CRASH in " + ", ".join(crashes)) if crashes else ""))
if not ok:
    rx = re.compile(r"CRASH|  r[a-z0-9]+ |stack:|session:|did not quit|pid==")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-16:]:
            print(l[1:13] + l[19:250])
print("RESULT: %s%s" % ("PASS" if ok else "FAIL", (" (only host %d, only joiner %d, actors apart %d, things apart %d, health %d, parents %d)" % total) if total else ""))
sys.exit(0 if ok else 1)
