"""series.py [--level level] [--seconds N] [--every S] [--pad]

Where is each player, by each PC, through a whole run? Two test copies (the host on --level from its start, or
on the usual save; a joiner), and from the moment the joiner's body is there both games write their state every
--every seconds (default 4). The table at the end puts the two PCs' readings side by side - the host's place as
the host has it and as the joiner has it, the same for the joiner - with each one's health, and marks the rows
where a player is dead or a death menu is open (a dead player's body is a ragdoll each PC throws by itself).

  --pad   the host walks forward for 2 s every fourth sample (otherwise nobody moves)

Two things are compared for each player: the slot's "network position" (that is the player's CAMERA: in a scene
or after a death it leaves the body) and the place of the player's BODY, the entity itself, read from a dump of
each PC's world ("!world n") - "body" columns. The result line says how far apart the two PCs' readings of a
LIVING player were at the worst, cameras and bodies.
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


pad = "--pad" in a
level = opt("--level")
seconds = int(opt("--seconds", "200"))
every = float(opt("--every", "4"))

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    dst = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst, exist_ok=True)
    shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_hunter_start"), os.path.join(dst, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

for c in (host, client):
    for f in os.listdir(c.dir) if os.path.isdir(c.dir) else []:
        if re.match(r"world-\d+\.txt$", f):
            os.remove(os.path.join(c.dir, f))
count = int((seconds - 75) / every)
steps_h, steps_c = "", ""
for i in range(count):
    walk = pad and i % 4 == 1
    if walk:
        steps_h += " | +%g: !pad 0 0 32000 2000 | +2: !world %X | +0.1: !net" % (every - 2.2, i)
    else:
        steps_h += " | +%g: !world %X | +0.1: !net" % (every - 0.1, i)
    steps_c += " | +%g: !world %X | +0.1: !net" % (every - 0.1, i)
ph = host.start(seconds, begin + " | ?pid==" + steps_h, extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid==" + steps_c, bind_port=1001, autostart=False, after=(host, ph),
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

STATE = re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+)\].*net \[lab\]: network game (\d+), .*local slot (-?\d+), players (\d+), level (\w+), "
                   r"body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)(.*)")
SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(peer|-)\(([-\d.]+),([-\d.]+),([-\d.]+)\)h(-?[\d.]+)")
CLOCK = re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+)\]")


def clock(line):
    m = CLOCK.match(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0 if m else None


def states(copy):
    out = []
    for l in copy.log_lines():
        m = STATE.search(l)
        if m:
            slots = {int(s[0]): dict(actor=s[1], pos=tuple(float(x) for x in s[4:7]), health=float(s[7])) for s in SLOT.findall(m.group(13))}
            out.append(dict(t=clock(l), body=m.group(9), body_pos=tuple(float(x) for x in m.group(10, 11, 12)), slots=slots))
    return out


def world(copy, n):
    """entity id -> its place, from the copy's n-th world dump"""
    out = {}
    path = os.path.join(copy.dir, "world-%d.txt" % n)
    if os.path.exists(path):
        for l in open(path, encoding="latin1"):
            f = l.rstrip("\n").split("\t")
            if len(f) >= 7:
                try:
                    out[int(f[0], 16)] = (float(f[4]), float(f[5]), float(f[6]))
                except ValueError:
                    pass
    return out


def marks(copy, text):
    return [clock(l) for l in copy.log_lines() if text in l]


def dist(p, q):
    return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5


hs, cs = states(host), states(client)
t0 = hs[0]["t"] if hs else 0
menus = [(t, "host") for t in marks(host, "Activate menu [die_in_game]")] + [(t, "joiner") for t in marks(client, "Activate menu [die_in_game]")]
events = sorted([(t, "the %s's death menu opens" % who) for t, who in menus]
                + [(t, "the host says: player 1 died") for t in marks(host, "session: player 1 died")]
                + [(t, "a new body for the joiner") for t in marks(host, "pid==1, eid==")[1:]]
                + [(t, "the host: Hud disable (a scene)") for t in marks(host, "Hud disable called")])
print("  time   host's camera: by host / by joiner            apart | joiner's camera: by joiner / by host         apart | bodies apart h / j | health h/j")
worst = [0.0, 0.0]
worst_body = [0.0, 0.0]
dead_rows = 0
for n, h in enumerate(hs):
    c = min(cs, key=lambda s: abs(s["t"] - h["t"])) if cs else None
    while events and events[0][0] <= h["t"]:
        print("  %5.1f  -- %s" % (events[0][0] - t0, events[0][1]))
        events.pop(0)
    if not c or abs(c["t"] - h["t"]) > every / 2 + 1 or 0 not in h["slots"] or 1 not in h["slots"] or 0 not in c["slots"] or 1 not in c["slots"]:
        print("  %5.1f  (no reading of the joiner's PC near this moment)" % (h["t"] - t0))
        continue
    d0, d1 = dist(h["slots"][0]["pos"], c["slots"][0]["pos"]), dist(h["slots"][1]["pos"], c["slots"][1]["pos"])
    hh, jh = h["slots"][0]["health"], h["slots"][1]["health"]
    host_dead = hh <= 0 or any(t <= h["t"] for t, who in menus if who == "host")
    joiner_dead = jh <= 0.05 or c["slots"][1]["health"] <= 0
    if not host_dead:
        worst[0] = max(worst[0], d0)
    if not joiner_dead:
        worst[1] = max(worst[1], d1)
    dead_rows += host_dead or joiner_dead
    # the bodies themselves, from the two world dumps of this sample (the sample's number is the row's)
    wh, wc = world(host, n), world(client, n)
    ids = [int(h["slots"][0]["actor"], 16), int(h["slots"][1]["actor"], 16)]
    b = [dist(wh[i], wc[i]) if i in wh and i in wc else None for i in ids]
    if b[0] is not None and not host_dead:
        worst_body[0] = max(worst_body[0], b[0])
    if b[1] is not None and not joiner_dead:
        worst_body[1] = max(worst_body[1], b[1])
    fmt = lambda p: "(%7.2f,%6.2f,%7.2f)" % p
    show = lambda v: "%5.2f" % v if v is not None else "  ?  "
    print("  %5.1f  %s %s %5.2f | %s %s %5.2f |   %s / %s   | %5.2f / %5.2f%s%s" % (
        h["t"] - t0, fmt(h["slots"][0]["pos"]), fmt(c["slots"][0]["pos"]), d0, fmt(c["slots"][1]["pos"]), fmt(h["slots"][1]["pos"]), d1,
        show(b[0]), show(b[1]), hh, jh, "   HOST DEAD" if host_dead else "", "   JOINER DEAD" if joiner_dead else ""))
crashes = [c.name for c in (host, client) if any("CRASH " in l for l in c.log_lines())]
print("ended: %s; fell: %s; rows: %d, of them with a dead player: %d" % (codes, ", ".join(crashes) or "nobody", len(hs), dead_rows))
print("RESULT: living players between the two PCs at the worst: cameras %.2f m (the host) and %.2f m (the joiner); "
      "bodies %.2f m (the host) and %.2f m (the joiner)" % (worst[0], worst[1], worst_body[0], worst_body[1]))
