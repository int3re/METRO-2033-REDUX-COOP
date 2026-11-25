"""smoke.py [--save name] [--level level] [--seconds N] [--no-pad] [--host-then steps] [--client-then steps]
            [--show regex]

The co-op smoke test: two test copies on this PC, the mod's own session logic ([net] mode = host / join), a
scripted gamepad on each side, and a list of facts read from the two logs. Prints every check and
"RESULT: PASS (n checks)" or "RESULT: FAIL (k of n)"; the exit code is 0 only on PASS.

The host starts from a save (default: re/lab-saves/2033_hunter_start, the checkpoint right after the intro of
l01_hunter, where the player can walk) or, with --level, from the start of a level.

--host-then / --client-then: script steps run on that side after the walks (" | " between steps, as in [lab]
script); --show: print the lines of both logs that match the regex.

Checks:
  1  both games ended by themselves (exit code 0)
  2  no crash in either log
  3  the host hosted the level and a player joined
  4  the joiner loaded the host's level and is player 1
  5  the joiner's body arrived on both sides with the same id, and the joiner controls it
  6  the host's position is the same on both sides at the end
  7  the joiner's position is the same on both sides at the end
  8  the host's walk (pad) moved its body, and the joiner saw it move      (unless --no-pad)
  9  the joiner's walk (pad) moved its body, and the host saw it move      (unless --no-pad)
"""
import os, re, shutil, sys, time
from lab import Copy, wait, wait_hosting, require_memory, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


no_pad = "--no-pad" in a
level = opt("--level")
save = opt("--save", "hunter_start")
seconds = int(opt("--seconds", "110"))
host_then, client_then = opt("--host-then", ""), opt("--client-then", "")
show = opt("--show")
EPS = float(opt("--eps", "0.15"))  # how far apart "the same place" may be (a host that a scene is moving: 0.5)

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")

if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    src = os.path.join(REPO, "re", "lab-saves", "2033_" + save)
    dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

# After the joiner's body is there: state, the host walks 3 s, state, (the joiner walks meanwhile + 6 s), state.
walk = "" if no_pad else " | +0.5: !pad 0 0 32000 3000 | +4: !net"
then = lambda steps: (" | " + steps) if steps else ""
# (a run without walking prints its state once more a second before the last: how fast a scene or a ride moves)
before_last = lambda t: (" | %.1f: !net" % (t - 1)) if no_pad else ""
host_script = begin + " | ?pid== | +4: !net" + walk + then(host_then) + before_last(seconds - 6) + " | %d: !net" % (seconds - 6)
walk = "" if no_pad else " | +0.5: !pad 0 0 32000 3000 | +4: !net"

t_host = time.time()
ph = host.start(seconds, host_script, extra="[net]\nmode = host")
# The joiner is started when the host hosts its level (however long that level loads), and ends with the host:
# its last "!net" and its end are counted on the host's clock.
wait_hosting(host, ph)
late = time.time() - t_host
c_last_at = max(30.0, seconds - 6 - late)
client_script = ("0.3: >stop_video | 8: !finish | ?pid== | +10: !net" + walk + then(client_then)
                 + before_last(c_last_at) + " | %.1f: !net" % c_last_at)
pc = client.start(max(36, int(round(seconds - late))), client_script, bind_port=1001, autostart=False,
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

STATE = re.compile(r"net \[lab\]: network game (\d+), host (\w+), local slot (-?\d+), players (\d+), level (\w+), "
                   r"body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)(.*)")
SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(peer|-)\(([-\d.]+),([-\d.]+),([-\d.]+)\)(?:h(-?[\d.]+))?")


def states(copy):
    out = []
    for l in copy.log_lines():
        m = STATE.search(l)
        if not m:
            continue
        slots = {int(s[0]): dict(actor=s[1], state=int(s[2]), pos=tuple(float(x) for x in s[4:7]),
                                 health=float(s[7]) if s[7] else 1.0)
                 for s in SLOT.findall(m.group(10))}
        out.append(dict(game=int(m.group(1)), host=m.group(2), local=int(m.group(3)), players=int(m.group(4)),
                        level=m.group(5), body=m.group(6), pos=tuple(float(x) for x in m.group(7, 8, 9)), slots=slots))
    return out


def has(copy, text):
    return any(text in l for l in copy.log_lines())


def near(p, q, eps=None):
    eps = EPS if eps is None else eps
    return p is not None and q is not None and all(abs(x - y) <= eps for x, y in zip(p, q))


def moved(p, q, least=0.3):
    return p is not None and q is not None and sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5 >= least


def slot_pos(st, i):
    return st["slots"].get(i, {}).get("pos") if st else None


hs, cs = states(host), states(client)
h_last, c_last = (hs[-1] if hs else None), (cs[-1] if cs else None)
# the last one: a level change gives the joiner a new body
pid = lambda copy: next((re.search(r"pid==1, eid==(\d+)", l).group(1) for l in reversed(copy.log_lines())
                         if "pid==1, eid==" in l), None)

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
crashes = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not crashes, ", ".join(crashes)))
checks.append(("the host hosted the level and a player joined",
               has(host, "session: hosting the level") and has(host, "[net] incoming connection"), ""))
checks.append(("the joiner loaded the host's level and is player 1",
               bool(c_last) and c_last["local"] == 1 and c_last["level"] == "loaded" and c_last["game"] == 1, ""))
hp, cp = pid(host), pid(client)
checks.append(("the joiner's body arrived on both sides and the joiner controls it",
               hp is not None and hp == cp and bool(c_last) and int(c_last["body"], 16) == int(cp), "%s / %s" % (hp, cp)))
same0, same1 = near(slot_pos(h_last, 0), slot_pos(c_last, 0)), near(slot_pos(h_last, 1), slot_pos(c_last, 1))
note = ""
if no_pad and not (same0 and same1) and len(hs) >= 2 and len(cs) >= 2:
    # A player whom a scene or a ride is moving (l30: the host's camera flies through the opening scene at 3 m a
    # second while its body sits still): the two PCs print their state up to half a second apart, so the two
    # readings may differ by the way made in that time. The speed is taken from the two last states, a second
    # apart, on each PC.
    def speed(sts, i):
        p, q = slot_pos(sts[-2], i), slot_pos(sts[-1], i)
        return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5 if p and q else 0.0
    v0, v1 = max(speed(hs, 0), speed(cs, 0)), max(speed(hs, 1), speed(cs, 1))
    if v0 > 0.2 or v1 > 0.2:
        same0 = near(slot_pos(h_last, 0), slot_pos(c_last, 0), EPS + 0.8 * v0)
        same1 = near(slot_pos(h_last, 1), slot_pos(c_last, 1), EPS + 0.8 * v1)
        note = "   moving at %.1f / %.1f m a second: 0.8 s of that way is allowed" % (v0, v1)
if no_pad and not (same0 and same1) and len(hs) >= 2 and all(slot_pos(st, i) for st in (h_last, c_last) for i in (0, 1)):
    # Nobody walks in this run, yet the host is not where it was: a ride or a scene carries the players (the
    # handcar of l03 does 10 m a second, and the two PCs print their state up to half a second apart). Then the
    # two PCs must agree on where the players are relative to each other, within 1.2 m.
    if moved(hs[0]["pos"], h_last["pos"], 3.0):
        rel = lambda st: tuple(a - b for a, b in zip(slot_pos(st, 1), slot_pos(st, 0)))
        if near(rel(h_last), rel(c_last), max(EPS, 1.2)):
            same0 = same1 = True
            note = "   carried by a ride or a scene: judged by the place of one player from the other"
# A dead player is not compared: the "position" of a slot is that player's camera, and a dead player's camera is
# the game's death track on its own PC (it goes on while the death menu holds that game paused and nothing more
# is sent - l36_ostankino, a host killed by a monster: 4 m between the two PCs' readings, with both living
# players within centimetres of each other all through the run; tools/lab/series.py shows it sample by sample).
host_dead = has(host, "Activate menu [die_in_game]") or any(st and st["slots"].get(0, {}).get("health", 1) <= 0 for st in (h_last, c_last))
joiner_dead = has(client, "Activate menu [die_in_game]") or any(st and st["slots"].get(1, {}).get("health", 1) <= 0 for st in (h_last, c_last))
if host_dead and not same0:
    same0, note0 = True, "   the host is dead: its death camera is not compared"
else:
    note0 = note
if joiner_dead and not same1:
    same1, note1 = True, "   the joiner is dead: its death camera is not compared"
else:
    note1 = note
checks.append(("the host's position is the same on both sides",
               same0, "%s / %s%s" % (slot_pos(h_last, 0), slot_pos(c_last, 0), note0)))
checks.append(("the joiner's position is the same on both sides",
               same1, "%s / %s%s" % (slot_pos(h_last, 1), slot_pos(c_last, 1), note1)))
if not no_pad:
    # host states: [before walk, after walk, last]; client states: [before its walk, after its walk, last]
    h0, h1 = (hs[0], hs[1]) if len(hs) >= 3 else (None, None)
    c0, c1 = (cs[0], cs[1]) if len(cs) >= 3 else (None, None)
    checks.append(("the host's walk moved its body and the joiner saw it",
                   bool(h0) and moved(h0["pos"], h1["pos"]) and near(slot_pos(h_last, 0), slot_pos(c_last, 0))
                   and moved(slot_pos(h0, 0), slot_pos(c_last, 0)),
                   "%s -> %s" % (h0["pos"] if h0 else None, h1["pos"] if h1 else None)))
    checks.append(("the joiner's walk moved its body and the host saw it",
                   bool(c0) and moved(c0["pos"], c1["pos"]) and near(slot_pos(h_last, 1), slot_pos(c_last, 1))
                   and moved(slot_pos(c0, 1), slot_pos(h_last, 1)),
                   "%s -> %s" % (c0["pos"] if c0 else None, c1["pos"] if c1 else None)))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed:
    for c in (host, client):
        print("---- %s: the last lines" % c.name)
        keep = re.compile(r"session:|net \[|net:|\[net\]|CRASH|  r[a-z0-9]+ |stack:|console: quit|pid==|lab: ")
        for l in [l for l in c.log_lines() if keep.search(l)][-14:]:
            print(l[1:13] + l[19:260])
if show:
    rx = re.compile(show)
    for c in (host, client):
        print("---- %s: lines with /%s/" % (c.name, show))
        for l in [l for l in c.log_lines() if rx.search(l)]:
            print(l[1:13] + l[19:260])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
