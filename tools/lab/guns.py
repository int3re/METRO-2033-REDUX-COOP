"""guns.py [--seconds N] [--show regex]

The weapons of a joined player, on l19_defence (the host comes with a shotgun, a revolver and an AK there and the
joiner gets copies of them): two test copies, the joiner takes each weapon into its hands (the actions the keys
1 / 2 / 3 send: "!act 19 / 1A / 40"), fires and reloads, the host changes weapons too. The numbers are read from the games' memory (rounds in a weapon: entity + 0x448,
rounds in an ammunition stack: entity + 0x4F4).

Checks:
  1  both games ended by themselves
  2  no crash, no "No ammo item" (the on-screen ammunition list found every stack)
  3  the joiner's inventory holds as many items as the host's own
  4  none of the joiner's three weapons lies in the world (each hangs on its body, on both PCs)
  5  the joiner starts with the main weapon (slot 16) in its hands: the first shots come from the AK
  6  the joiner reloads the AK from its own stack
  7  the joiner's shotgun (slot 1) fires and reloads
  8  the joiner's revolver (slot 2) fires and reloads
  9  the joiner gets the AK (slot 16) back
 10  the host changes to the shotgun and back to the AK (slot 16 in a network game)
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


seconds = int(opt("--seconds", "180"))
show = opt("--show")

# l19_defence, the body of player 1 made first: ids are the same in every run
HOST = dict(body="F107", ak="720A", shotgun="7509", revolver="7705", r545="390C", r12="110C", r44="1D0B")
GUEST = dict(body="7D0A", ak="A30A", shotgun="9E1E", revolver="A10C", r545="8906", r12="8208", r44="8606")


def read(ids):
    return " | +0.05: ".join(["!look %s 448 1" % ids[k] for k in ("ak", "shotgun", "revolver")] +
                             ["!look %s 4F0 1" % ids[k] for k in ("r545", "r12", "r44")])


def at(name, ids):
    return "!mark %s | +0.05: %s" % (name, read(ids))


# (a press shorter than a frame of a busy PC can be missed: the reload button is pressed twice, the second press
# does nothing when the first one worked)
FIRE, RELOAD = "!pad 0 0 0 700 0 0 0 255", "!pad 4000 0 0 300 | +3: !pad 4000 0 0 300"
g = GUEST
client_steps = " | ".join([
    "+5: " + at("start", g),
    "+0.3: " + FIRE, "+1.5: " + at("ak_fired", g),
    "+0.3: " + RELOAD, "+4.5: " + at("ak_reloaded", g),
    "+0.3: !act 19", "+5: " + FIRE, "+1.5: " + at("shotgun_fired", g),
    "+0.3: " + RELOAD, "+4.5: " + at("shotgun_reloaded", g),
    "+0.3: !act 1A", "+5: " + FIRE, "+1.5: " + at("revolver_fired", g),
    "+0.3: " + RELOAD, "+4.5: " + at("revolver_reloaded", g),
    "+0.3: !act 40", "+5: " + FIRE, "+1.5: " + at("ak_again", g),
    "+0.2: !inv %s" % g["body"], "+0.2: !ent %s" % g["ak"], "+0.1: !ent %s" % g["shotgun"], "+0.1: !ent %s" % g["revolver"],
])
h = HOST
host_steps = " | ".join([
    "+5: " + at("start", h),
    # away from the joiner first: both stand on one spot after a join
    "+0.3: !pad 0 0 32000 1000",
    "+2: !act 19", "+5: " + FIRE, "+1.5: " + at("shotgun_fired", h),
    "+0.3: !act 40", "+5: " + FIRE, "+1.5: " + at("ak_again", h),
    "+0.2: !inv %s" % h["body"], "+0.2: !ent %s" % g["ak"], "+0.1: !ent %s" % g["shotgun"], "+0.1: !ent %s" % g["revolver"],
])

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map 2033\l19_defence"
ph = host.start(seconds, begin + " | ?pid== | " + host_steps + " | +1: !net", extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== | " + client_steps + " | +1: !net", bind_port=1001,
                  autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

LOOK = re.compile(r"net: entity ([0-9A-F]+) \+ ([0-9A-F]+): ([0-9A-F]{16})")


def marks(copy, ids):
    """{mark: {ak, shotgun, revolver, r545, r12, r44}} - the numbers read after each mark."""
    back = {v: k for k, v in ids.items()}
    out, cur = {}, None
    for l in copy.log_lines():
        if "lab: mark " in l:
            cur = l.split("lab: mark ", 1)[1].strip()
            out[cur] = {}
            continue
        m = LOOK.search(l)
        if m and cur and m.group(1) in back:
            v = int(m.group(3), 16)
            out[cur][back[m.group(1)]] = (v >> 32) if m.group(2) == "4F0" else (v & 0xFFFFFFFF)
    return out


def items(copy, body):
    for l in copy.log_lines():
        m = re.search(r"the inventory of %s \(class \+[0-9A-F]+\) holds (\d+) items" % body, l)
        if m:
            return int(m.group(1))
    return None


def hangs(copy, ident):
    for l in reversed(copy.log_lines()):
        m = re.search(r"net: entity %s '[^']*' flags [0-9A-F]+, hangs on ([0-9A-F]+)" % ident, l)
        if m:
            return m.group(1)
    return None


def has(copy, text):
    return any(text in l for l in copy.log_lines())


def val(d, mark, key):
    return d.get(mark, {}).get(key)


def less(d, before, after, key):
    x, y = val(d, before, key), val(d, after, key)
    return x is not None and y is not None and y < x


def more(d, before, after, key):
    x, y = val(d, before, key), val(d, after, key)
    return x is not None and y is not None and y > x


gm, hm = marks(client, GUEST), marks(host, HOST)
row = lambda d, m: " ".join("%s=%s" % (k, d.get(m, {}).get(k)) for k in ("ak", "shotgun", "revolver", "r545", "r12", "r44"))

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ") or has(c, "No ammo item")]
checks.append(("no crash, no missing ammunition stack", not bad, ", ".join(bad)))
gi, hi = items(client, GUEST["body"]), items(host, HOST["body"])
checks.append(("the joiner's inventory holds as many items as the host's own", gi is not None and gi == hi, "%s / %s" % (gi, hi)))
where = [hangs(c, GUEST[k]) for c in (host, client) for k in ("ak", "shotgun", "revolver")]
checks.append(("the joiner's three weapons hang on its body on both PCs", all(w == GUEST["body"] for w in where), " ".join(map(str, where))))
checks.append(("the joiner starts with the AK in its hands", less(gm, "start", "ak_fired", "ak")
               and val(gm, "start", "revolver") == val(gm, "ak_fired", "revolver"), row(gm, "start") + "  ->  " + row(gm, "ak_fired")))
checks.append(("the joiner reloads the AK from its own stack", more(gm, "ak_fired", "ak_reloaded", "ak")
               and less(gm, "ak_fired", "ak_reloaded", "r545"), row(gm, "ak_reloaded")))
checks.append(("the joiner's shotgun fires and reloads", less(gm, "ak_reloaded", "shotgun_fired", "shotgun")
               and more(gm, "shotgun_fired", "shotgun_reloaded", "shotgun") and less(gm, "shotgun_fired", "shotgun_reloaded", "r12"),
               row(gm, "shotgun_fired") + "  ->  " + row(gm, "shotgun_reloaded")))
checks.append(("the joiner's revolver fires and reloads", less(gm, "shotgun_reloaded", "revolver_fired", "revolver")
               and more(gm, "revolver_fired", "revolver_reloaded", "revolver") and less(gm, "revolver_fired", "revolver_reloaded", "r44"),
               row(gm, "revolver_fired") + "  ->  " + row(gm, "revolver_reloaded")))
checks.append(("the joiner gets the AK back", less(gm, "revolver_reloaded", "ak_again", "ak"), row(gm, "ak_again")))
checks.append(("the host changes to the shotgun and back to the AK", less(hm, "start", "shotgun_fired", "shotgun")
               and less(hm, "shotgun_fired", "ak_again", "ak"), row(hm, "start") + "  ->  " + row(hm, "shotgun_fired") + "  ->  " + row(hm, "ak_again")))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|No ammo|own body|inventory action|lab: mark|did not quit")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-40:]:
            print(l[1:13] + l[19:220])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
