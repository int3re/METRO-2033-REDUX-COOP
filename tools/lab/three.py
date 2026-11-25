"""three.py [--level level] [--seconds N] [--show regex]

Three players: the host and two joined games (three test copies on this PC: test-m33, test-m33b, test-m33c).
The host starts from the hunter_start save (or --level), both joiners connect, get bodies, each walks.

Checks:
  1  the three games ended by themselves
  2  no crash
  3  the host has three players and both joiners play (state 5)
  4  the joiners are players 1 and 2 and each controls its own body
  5  each body has the same id on all three PCs
  6  each player's position is the same on all three PCs at the end
  7  each joiner's walk moved its body and the two others saw it
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
seconds = int(opt("--seconds", "150"))
show = opt("--show")

require_memory(3)
host, c1, c2 = Copy("test-m33"), Copy("test-m33b"), Copy("test-m33c")
if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    src = os.path.join(REPO, "re", "lab-saves", "2033_hunter_start")
    dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

end = " | %d: !net" % (seconds - 6)
host_script = begin + " | ?pid== | +30: !net" + end
# the second joiner comes 12 s after the first; each walks once both bodies are there
join = "0.3: >stop_video | %d: !finish | ?pid== | +%d: !net | +0.5: !pad 0 0 32000 %d | +4: !net" + end
ph = host.start(seconds, host_script, extra="[net]\nmode = host")
# (the joiners are started when the host hosts its level - see lab.Copy.start(after=...): three copies loading at
# once are slow, and a joiner that knocks before the host is ready gives up after a minute)
p1 = c1.start(seconds, join % (20, 30, 1500), bind_port=1001, autostart=False, after=(host, ph),
              extra="[net]\nmode = join\naddress = 127.0.0.1")
p2 = c2.start(seconds, join % (32, 22, 700), bind_port=1002, autostart=False, after=(host, ph),
              extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, p1, p2], seconds + 240)

STATE = re.compile(r"net \[lab\]: network game (\d+), host (\w+), local slot (-?\d+), players (\d+), level (\w+), "
                   r"body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)(.*)")
SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(?:peer|-)\(([-\d.]+),([-\d.]+),([-\d.]+)\)")


def states(copy):
    out = []
    for l in copy.log_lines():
        m = STATE.search(l)
        if not m:
            continue
        slots = {int(s[0]): dict(actor=s[1], state=int(s[2]), pos=tuple(float(x) for x in s[3:6])) for s in SLOT.findall(m.group(10))}
        out.append(dict(local=int(m.group(3)), players=int(m.group(4)), body=m.group(6),
                        pos=tuple(float(x) for x in m.group(7, 8, 9)), slots=slots))
    return out


def has(copy, text):
    return any(text in l for l in copy.log_lines())


def near(p, q, eps=0.2):
    return p is not None and q is not None and all(abs(x - y) <= eps for x, y in zip(p, q))


def moved(p, q, least=0.3):
    return p is not None and q is not None and sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5 >= least


copies = (host, c1, c2)
st = [states(c) for c in copies]
last = [s[-1] if s else None for s in st]
slot = lambda who, i, key: (last[who]["slots"].get(i, {}).get(key) if last[who] else None)

checks = []
checks.append(("the three games ended by themselves", codes == [0, 0, 0], str(codes)))
bad = [c.name for c in copies if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the host has three players and both joiners play", bool(last[0]) and last[0]["players"] == 3
               and slot(0, 1, "state") == 5 and slot(0, 2, "state") == 5, str(last[0]["slots"] if last[0] else None)))
locals_ = sorted(l["local"] for l in last[1:] if l)
own = all(l and l["body"] == l["slots"].get(l["local"], {}).get("actor") for l in last[1:])
checks.append(("the joiners are players 1 and 2 and each controls its own body", locals_ == [1, 2] and own, str(locals_)))
ids = [[slot(w, i, "actor") for w in range(3)] for i in range(3)]
checks.append(("each body has the same id on all three PCs", all(len(set(x)) == 1 and x[0] not in (None, "FFFF") for x in ids), str(ids)))
pos = [[slot(w, i, "pos") for w in range(3)] for i in range(3)]
checks.append(("each player's position is the same on all three PCs", all(near(x[0], x[1]) and near(x[0], x[2]) for x in pos), str(pos)))
walked = []
for w in (1, 2):
    s = st[w]
    me = last[w]["local"] if last[w] else None
    ok = len(s) >= 3 and moved(s[0]["pos"], s[1]["pos"]) and me is not None and all(
        moved(s[0]["slots"].get(me, {}).get("pos"), slot(o, me, "pos")) for o in range(3) if o != w)
    walked.append(ok)
checks.append(("each joiner's walk moved its body and the others saw it", all(walked), str(walked)))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail[:330] + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"session:|net \[lab|CRASH|  r[a-z0-9]+ |stack:|pid==|did not quit|own body")
    for c in copies:
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-16:]:
            print(l[1:13] + l[19:250])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
