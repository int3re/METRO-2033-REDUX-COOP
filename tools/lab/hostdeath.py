"""hostdeath.py [--seconds N] [--show regex]

The host is killed by a monster with a joined player in the game, then takes "load the last checkpoint" in the
game's own death menu.

The run starts from the save chase_fight (l03_chase, the handcar after the vision; a nosalis jumps on about 30 s
later and kills a player who does nothing in about 50 s). The host writes a checkpoint right after loading, a
second copy joins, both sit still. When the game opens its death menu (its line "Activate menu [die_in_game]")
the host's pad presses A two and a half seconds later: the menu's first line. (Nothing here goes by the clock:
how long the level loads and when the nosalis is done differs from run to run. Between the menu and the press
the script only writes the state down - with a picture taken in between, or with the press at 4 s and 8 s, the
menu did not take it, twice of twice; at 2.5 s with nothing in between it does.)

Checks:
  1  both games ended by themselves
  2  no crash
  3  the joiner got its body before the fight
  4  the host died (its health fell to zero or below)
  5  the host's game loaded its checkpoint after the press
  6  the joiner was taken along (told to load, plays again)
  7  after the load the host lives again, and the joiner controls a body at the end

Pictures of both sides go to re/shots/hostdeath/.
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


seconds = int(opt("--seconds", "240"))
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
src = os.path.join(REPO, "re", "lab-saves", "2033_chase_fight")
dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
os.makedirs(dst_dir, exist_ok=True)
shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))

look = lambda side, n: " | +0.1: !near 8 | +0.1: !shot %s%03d" % (side, n) if side == "h" else " | +0.1: !shot %s%03d" % (side, n)
# the host: its state once the joiner is in; when its death menu opens (and the press); after the checkpoint is
# loaded and the joiner has a body again
host_steps = (" | +2: !net" + look("h", 1)
              + " | ?Activate menu [die_in_game] | +0.2: !net | +2.3: !pad 1000 0 0 300"
              + " | ?pid== | +5: !net" + look("h", 3) + " | +10: !net" + look("h", 4))
client_steps = (" | +2: !net" + look("c", 1)
                + " | ?pid== | +5: !net" + look("c", 3) + " | +10: !net" + look("c", 4))
ph = host.start(seconds, "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"
                         " | ?loader_finish: mapstate_running | +5: gamesave auto_save | ?pid== " + host_steps + " | +1: !net",
                extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== " + client_steps + " | +1: !net", bind_port=1001,
                  autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

out_dir = os.path.join(REPO, "re", "shots", "hostdeath")
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
for c, side in ((host, "host"), (client, "client")):
    files = shots(c, out_dir, 1.0, 0.6)
    if files:
        print("%s: %d pictures, sheet %s" % (side, len(files), sheet(files, os.path.join(out_dir, "sheet_%s.png" % side))))

SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(?:peer|-)\([-\d.,]+\)h(-?[\d.]+)")
STATE = re.compile(r"net \[lab\]: network game (\d+), host (\w+), local slot (-?\d+), players (\d+), level (\w+), body ([0-9A-F]+)")


def lines(copy, text):
    return [l for l in copy.log_lines() if text in l]


def slots(copy):
    """per "!net" line: {slot: (actor, state, health)} and the body this PC controls"""
    out = []
    for l in copy.log_lines():
        m = STATE.search(l)
        if m:
            out.append((m.group(6), {int(s): (actor, int(state), float(h)) for s, actor, state, h in SLOT.findall(l)}))
    return out


hs, cs = slots(host), slots(client)
host_health = [s[1].get(0, ("", 0, 9.0))[2] for s in hs]
press = next((i for i, l in enumerate(host.log_lines()) if "pad" in l and "1000" in l and "lab" in l), None)
loads = [i for i, l in enumerate(host.log_lines()) if "the host loads a save" in l or "loading now" in l]
told = lines(host, "told to load")
bodies = [m.group(1) for l in client.log_lines() for m in [re.search(r"pid==1, eid==(\d+)", l)] if m]
last_h, last_c = (hs[-1] if hs else None), (cs[-1] if cs else None)

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if lines(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the joiner got its body before the fight", len(bodies) >= 1, str(bodies)))
checks.append(("the host died", any(h <= 0 for h in host_health), str(host_health)))
checks.append(("the host's game loaded its checkpoint after the press", bool(loads), "%d load lines" % len(loads)))
checks.append(("the joiner was taken along", bool(told) and len(bodies) >= 2, "told %d times, bodies %s" % (len(told), bodies)))
# (nobody shoots in this test: the nosalis comes again after the checkpoint and kills the host once more)
died = next((i for i, h in enumerate(host_health) if h <= 0), None)
alive = died is not None and any(h > 0 for h in host_health[died:])
own = bool(last_c) and last_c[1].get(1, ("", 0, 0.0))[0] == last_c[0] and last_c[1].get(1, ("", 0, 0.0))[1] == 5
checks.append(("after the load the host lives again and the joiner controls a body at the end", alive and own,
               "host health %s / joiner %s" % (host_health, last_c if last_c else None)))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail[:300] + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|loads a save|told to load|loading now|did not quit|pid==|rides|rail|link:")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l) and "cont_rail" not in l][-28:]:
            print(l[1:13] + l[19:240])
print("pictures: %s" % out_dir)
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
