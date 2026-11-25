"""aside.py [--level level] [--seconds N] [--show regex]

Does a player who joins stand clear of the host? The host starts l19_defence, walks forward for three seconds
(its way is remembered), stops; then a second copy joins. The host sends its last steps with the new body, and
the joined game puts its body on a point of that way 1.2-10 m behind the host.

Checks:
  1  both games ended by themselves
  2  no crash
  3  the host had walked before the joiner came (more than 2 m)
  4  the joined game got the host's way and stepped aside
  5  the two bodies are more than 1 m apart on the joiner's PC ...
  6  ... and on the host's PC
"""
import re, sys
from lab import Copy, wait, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level", r"2033\l19_defence")
seconds = int(opt("--seconds", "85"))
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
ph = host.start(seconds, begin + " | ?^map | ?mapstate_running | +1.5: !net | +0.5: !pad 0 0 32000 3000 | +4.5: !net | ?pid== | +4: !net | +1: !net",
                extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== | +4: !net | +1: !net", bind_port=1001, autostart=False, after=(host, ph),
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

STATE = re.compile(r"net \[lab\]: network game \d+, host \w+, local slot (-?\d+), players (\d+), level \w+, body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)")
SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(?:peer|-)\(([-\d.]+),([-\d.]+),([-\d.]+)\)")


def samples(copy):
    out = []
    for l in copy.log_lines():
        m = STATE.search(l)
        if m:
            out.append((tuple(float(x) for x in m.group(4, 5, 6)), {int(s[0]): tuple(float(x) for x in s[3:6]) for s in SLOT.findall(l)}))
    return out


def dist(p, q):
    return ((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5 if p and q else -1.0


has = lambda copy, text: any(text in l for l in copy.log_lines())
hs, cs = samples(host), samples(client)
walked = dist(hs[0][0], hs[1][0]) if len(hs) >= 2 else -1.0
gap_c = dist(cs[-1][1].get(0), cs[-1][1].get(1)) if cs else -1.0
gap_h = dist(hs[-1][1].get(0), hs[-1][1].get(1)) if hs else -1.0
aside = next((l[19:200] for l in client.log_lines() if "steps" in l and "aside" in l), "")

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the host had walked before the joiner came", walked > 2.0, "%.1f m" % walked))
checks.append(("the joined game got the host's way and stepped aside", has(client, "the host's way came with the body") and bool(aside), aside))
checks.append(("the two bodies are apart on the joiner's PC", gap_c > 1.0, "%.2f m" % gap_c))
checks.append(("the two bodies are apart on the host's PC", gap_h > 1.0, "%.2f m" % gap_h))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|did not quit|pid==|aside|way came|stays where|net \[lab")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-16:]:
            print(l[1:13] + l[19:250])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
