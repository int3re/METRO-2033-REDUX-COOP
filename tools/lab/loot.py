"""loot.py [--joiner-first]

Loot in a session: every player takes their own. On l00_intro the host gets through the level's opening, a second
copy joins ~55 s later; one player takes the loot of the cabinet metal_locker_0000 by the gate (four packs of 5.45
and a medkit), then the other player takes the same cabinet's loot.

The host takes as a player does (the use blocks open the cabinet, Use at what lies inside takes all of it). The
joined copy cannot use anything in the lab (its window is never the active one: Use finds no target there), so it
sends the five "take" events itself - what its Use would send; the host takes them on trust, as it now takes every
joined player's take.

Checks: both ended, no crash; the first taker took the five things; after that the other PC's cabinet still holds
them; the second taker took the five things too; neither PC took them twice; afterwards the cabinet is empty on
both PCs.
"""
import os, re, sys, time
from lab import Copy, wait, require_memory, REPO, shots, sheet

a = sys.argv[1:]
seconds = 190
joiner_first = "--joiner-first" in a

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
for c in (host, client):
    for f in os.listdir(c.dir) if os.path.isdir(c.dir) else []:
        if re.match(r"world-\d+\.txt$", f):
            os.remove(os.path.join(c.dir, f))

LOOT = ["1A6", "6300", "175", "1B8", "2BD"]
host_take = (" | +0.5: !move FFFF 223.3 -13.9 -161.7 | +3: !usename metal_locker_0000 1 | +1: !usename metal_locker_0000 0"
             " | +3.5: !aim 2BD 2500 0 0 | +0.4: !pad 4000 0 0 600 | +3.5: !aim 175 2000 0 0 | +0.2: !mark take | +0.2: !pad 4000 0 0 600"
             " | +3: !mark taken | +0.1: !shot taken | +0.2: !inv FFFF")
client_take = (" | +0.5: !move FFFF 223.3 -13.9 -162.6 | +3: !usename metal_locker_0000 1 | +1: !usename metal_locker_0000 0 | +2: !mark take"
               + "".join(" | +0.1: !take %s 0" % i for i in LOOT) + " | +3: !mark taken | +0.2: !inv FFFF")
h = ("0.3: >stop_video | +0.5: disconnect | +0.5: map 2033\\l00_intro | ?loader_finish: mapstate_running | +1: !takes 300 | +12: !pad 4000 0 0 600"
     " | ?pid== | +3: !mark joined")
# (the joined game's own script of the level's opening waits for its player to pick the gear up, as the host's did)
c = "0.3: >stop_video | 20: !finish | ?pid== | +1: !takes 300 | +2: !pad 4000 0 0 600 | +3: !mark joined"
if joiner_first:
    c += " | +2: !mark first" + client_take + " | +3: !world 1 | +20: !mark second | +12: !world 2"
    h += " | +2: !mark wait | +23: !world 1 | +1: !mark second" + host_take + " | +3: !world 2"
else:
    h += " | +2: !mark first" + host_take + " | +3: !world 1 | +4: !mark second | +16: !world 2"
    c += " | +2: !mark wait | +22: !world 1 | +1: !mark second" + client_take + " | +3: !world 2"
ph = host.start(seconds, h, extra="[net]\nmode = host")
time.sleep(55)  # the host gets through the level's opening alone
pc = client.start(seconds - 55, c, bind_port=1001, autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)
for x in (host, client):
    files = shots(x, os.path.join(REPO, "re", "shots", "loot-" + x.name))
    if files:
        print("%s: %s" % (x.name, sheet(files, os.path.join(REPO, "re", "shots", "loot-" + x.name, "sheet.png"), columns=4, width=400)))


def world(copy, n):
    out = {}
    p = os.path.join(copy.dir, "world-%d.txt" % n)
    if os.path.exists(p):
        for l in open(p, encoding="latin1"):
            f = l.rstrip("\n").split("\t")
            if len(f) >= 11:
                out[f[0]] = (f[2], f[9], f[10])
    return out


def in_cabinet(w):
    return sorted(i for i in LOOT if i in w and w[i][1] == "1703")


lines = {x.name: x.log_lines() for x in (host, client)}
has = lambda copy, text: sum(text in l for l in lines[copy.name])


def body_of(copy):
    for l in lines[copy.name]:
        m = re.search(r"net: the inventory of ([0-9A-F]+) ", l)
        if m:
            return m.group(1)
    return None


def takes(copy, by):
    """which of the cabinet's things the body `by` took on this PC"""
    out = []
    for l in lines[copy.name]:
        m = re.search(r"lab: ([0-9A-F]+) '[^']*' takes ([0-9A-F]+) '", l)
        if m and m.group(1) == by and m.group(2) in LOOT:
            out.append(m.group(2))
    return sorted(set(out))


hb, cb = body_of(host), body_of(client)
first, second = (client, host) if joiner_first else (host, client)
fb, sb = (cb, hb) if joiner_first else (hb, cb)
w1_other = world(second, 1)
h2, c2 = world(host, 2), world(client, 2)
checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [x.name for x in (host, client) if has(x, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the first taker (%s) took the five things" % first.name, len(takes(first, fb)) == 5, "%s" % takes(first, fb)))
checks.append(("after that the other PC's cabinet still holds them", len(in_cabinet(w1_other)) == 5, "%s" % in_cabinet(w1_other)))
checks.append(("the second taker (%s) took the five things too" % second.name, len(takes(second, sb)) == 5, "%s" % takes(second, sb)))
checks.append(("no PC took them for the other player as well", not takes(first, sb) and not takes(second, fb),
               "on %s by %s: %s; on %s by %s: %s" % (first.name, sb, takes(first, sb), second.name, fb, takes(second, fb))))
checks.append(("the cabinet is empty on both PCs at the end", bool(h2) and bool(c2) and not in_cabinet(h2) and not in_cabinet(c2),
               "host %s / joiner %s" % (in_cabinet(h2), in_cabinet(c2))))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail[:400] + "]") if detail else ""))
    failed += not ok
rx = re.compile(r"CRASH|stack:|lab: mark|loot: |let through|act event runs|takes [0-9A-F]+ '(ammo_5_45|medkit_0010)|aimed|inventory of")
for x in (host, client):
    print("---- %s" % x.name)
    for l in [l for l in lines[x.name] if rx.search(l) and "taken out by" not in l][-40:]:
        print(l[1:13] + l[19:300])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
