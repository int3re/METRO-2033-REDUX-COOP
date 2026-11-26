"""lockers.py [--level level] [--host "name k; name k"] [--client "name k; ..."] [--seconds N]

"Use" by the THING, not by a number in a list: a cabinet, a gate's wheel, a door used on one PC is used on the
others. The host starts a level from its beginning ("map"; default l00_intro, which has metal cabinets and a
gate wheel), a second copy joins; each side then uses things by their entity names ("!usename name k": the k-th
of the use blocks waiting on that thing, counted from the one made last - a cabinet has two).

The PCs' lists of use blocks may differ (the joined game makes them twice: from the level, then from the host's
save): that is the case this test is for.

Checks: both ended, no crash; every use reached the other PC and was fired there ("fired here too", none "has
no such use block"); the used things are in the same state on both PCs afterwards (flags and place, "!world").
"""
import os, re, shutil, sys
from lab import Copy, wait, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level", r"2033\l00_intro")
host_uses = [u.split() for u in opt("--host", "metal_locker_0000 1; metal_locker_0000 0; gate_wheel_0004 0").split(";") if u.strip()]
client_uses = [u.split() for u in opt("--client", "metal_locker_0006 1; metal_locker_0006 0").split(";") if u.strip()]
seconds = int(opt("--seconds", "150"))

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
for c in (host, client):
    for f in os.listdir(c.dir) if os.path.isdir(c.dir) else []:
        if re.match(r"world-\d+\.txt$", f):
            os.remove(os.path.join(c.dir, f))


def using(uses, start):
    steps = " | +%g: !mark use" % start
    for name, k in uses:
        steps += " | +0.1: !usename %s %s | +3: !mark used %s" % (name, k, name)
    return steps


h = "0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?pid== | +3: !uses" % level + using(host_uses, 3) + " | +12: !world 1 | +1: !net"
c = "0.3: >stop_video | 20: !finish | ?pid== | +3: !uses" + using(client_uses, 3 + 3.1 * len(host_uses) + 2) + " | +6: !world 1 | +1: !net"
c = c.replace("| +6: !world 1", "| +%g: !world 1" % max(1.0, 3.1 * len(host_uses) + 15 - (3 + 3.1 * len(host_uses) + 2 + 3.1 * len(client_uses))))
ph = host.start(seconds, h, extra="[net]\nmode = host")
pc = client.start(seconds, c, bind_port=1001, autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)


def world(copy):
    out, names = {}, {}
    p = os.path.join(copy.dir, "world-1.txt")
    if os.path.exists(p):
        for l in open(p, encoding="latin1"):
            f = l.rstrip("\n").split("\t")
            if len(f) >= 11:
                out[int(f[0], 16)] = (f[2], tuple(float(x) for x in f[4:7]), f[10])
                names[f[10]] = int(f[0], 16)
    return out, names


has = lambda copy, text: sum(text in l for l in copy.log_lines())
count_blocks = lambda copy: [l[l.find("net: ") + 5:].strip() for l in copy.log_lines() if "use blocks," in l]
hw, hn = world(host)
cw, cn = world(client)
same_state = lambda x, y: (x is None and y is None) or (x and y and (int(x[0], 16) & ~0xE000) == (int(y[0], 16) & ~0xE000)
                                                        and all(abs(p - q) < 0.05 for p, q in zip(x[1], y[1])))
things = sorted({n for n, _ in host_uses + client_uses})
states = [(n, hw.get(hn.get(n, -1)), cw.get(cn.get(n, -1))) for n in things]

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [x.name for x in (host, client) if has(x, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the lists of use blocks (they may differ)", True, "host: %s; joiner: %s" % (count_blocks(host)[-1:], count_blocks(client)[-1:])))
# (since 0.4.3 a joined game does not play the host's uses: what they move comes to it as the host's motions)
checks.append(("every host's use reached the joined PC (left to the host's motions there)",
               has(client, "used a thing: left to the host's motions here") >= len(host_uses),
               "%d of %d" % (has(client, "used a thing: left to the host's motions here"), len(host_uses))))
checks.append(("every joiner's use was fired on the host's PC", has(host, "player 1 used a thing (waits on") >= len(client_uses)
               and has(host, "fired here too") >= len(client_uses) and not has(host, "has no such use block"),
               "fired %d of %d" % (has(host, "fired here too"), len(client_uses))))
checks.append(("the used things are in the same state on both PCs", bool(states) and all(same_state(x, y) for _, x, y in states),
               "; ".join("%s: host %s / joiner %s" % (n, x[:2] if x else None, y[:2] if y else None) for n, x, y in states)[:500]))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
rx = re.compile(r"CRASH|stack:|used a thing|lab uses|no such|has \d+ use blocks|no entity named|use blocks,")
for x in (host, client):
    print("---- %s" % x.name)
    for l in [l for l in x.log_lines() if rx.search(l)][-14:]:
        print(l[1:13] + l[19:230])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
