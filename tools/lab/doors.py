"""doors.py [--level level] [--seconds N]

The things a story character moves (a door Miller opens in a scene) move on every PC. The joined game's copy
of a character never reaches its "open", so the host tells which motion a thing started and the joined game
starts it on its copy - unless its own script did the same.

On l00_intro, host and joiner:
  - before the joiner comes in, the host's gate zatvor_0002 (400) starts "half_open_01": the joined game asks for
    the level's motions once its level runs and gets it;
  - after the join, the host alone starts Miller's door scene_door_0000 (1401) "l21_melnik_repka_use", as the
    scene does on the host only;
  - both start the hall door scene_hall_door_0000 (8900) "l_00_open" at the same moment, as their scripts would:
    the joined game must not start it twice;
  - the joiner alone starts the cabinet metal_locker_0000 (1703) "idle_open": a joined game does not tell the host
    (its own "use" goes to the host by its own way).

Checks: both ended, no crash; the joiner has 400 and 1401 "for the host" with the times the host has (within
0.6 s); 8900 plays once on the joiner (no second start); the host's 1703 stays as it was; the host sent a sane
number of notes.
"""
import os, re, sys
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
seconds = int(opt("--seconds", "110"))

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
look = " | +0.1: !motionof 400 | +0.1: !motionof 1401 | +0.1: !motionof 8900 | +0.1: !motionof 1703"
h = ("0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?loader_finish: mapstate_running | +2: !play 400 half_open_01"
     " | +0.5: !motions 200 | ?pid== | +8: !mark A" % level + look +
     " | +1: !play 1401 l21_melnik_repka_use | +3: !mark B" + look +
     " | +3: !mark C | +0.1: !play 8900 l_00_open | +1: !mark C2 | +0.1: !motionof 8900 | +3: !mark D" + look + " | +4: !mark E" + look + " | +1: !motions 1")
c = ("0.3: >stop_video | 20: !finish | ?pid== | +0.5: !motions 200 | +8: !mark A" + look +
     " | +1: !mark joiner-cabinet | +0.1: !play 1703 idle_open | +2.9: !mark B" + look +
     " | +3: !mark C | +0.1: !play 8900 l_00_open | +1: !mark C2 | +0.1: !motionof 8900 | +3: !mark D" + look + " | +4: !mark E" + look)
ph = host.start(seconds, h, extra="[net]\nmode = host")
pc = client.start(seconds, c, bind_port=1001, autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

rx_state = re.compile(r"lab: motion of ([0-9A-F]+) '[^']*': '([^']*)' \(#\d+\)( for the host| by this PC), ([\d.]+) s ago, time ([-\d.]+) of ([-\d.]+)")


def clock(l):
    h, m, s = l[1:13].split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def states(copy):
    """{mark: {id: (motion, who, time, length, wall clock)}}"""
    out, mark = {}, None
    for l in copy.log_lines():
        m = re.search(r"lab: mark (\S+)", l)
        if m:
            mark = m.group(1)
            continue
        m = rx_state.search(l)
        if m and mark:
            out.setdefault(mark, {})[int(m.group(1), 16)] = (m.group(2), m.group(3).strip(), float(m.group(5)), float(m.group(6)), clock(l))
    return out


def alike(x, y):
    """Two readings of one motion on two PCs, taken at different moments: the same place at the same moment? (A lab
    copy's game clock runs at 0.5-1.0 of real time, so the moments' gap is scaled by that much, 0.4 s either way.)"""
    if not (x and y and x[0] == y[0]):
        return False
    if x[2] >= x[3] - 0.01 and y[2] >= y[3] - 0.01:  # both at the end
        return True
    d, gap = y[2] - x[2], y[4] - x[4]
    lo, hi = sorted((0.5 * gap, 1.0 * gap))
    return lo - 0.4 <= d <= hi + 0.4


hs, cs = states(host), states(client)
has = lambda copy, text: sum(text in l for l in copy.log_lines())


def same(mark, id_, motion):
    x, y = hs.get(mark, {}).get(id_), cs.get(mark, {}).get(id_)
    ok = bool(x and y and x[0] == motion and y[1] == "for the host" and alike(x, y))
    return ok, "host %s / joiner %s" % (x[:3] if x else None, y[:3] if y else None)


checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [x.name for x in (host, client) if has(x, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
ok, d = same("A", 0x400, "half_open_01")
checks.append(("the gate the host opened before the join is open on the joiner too (asked for)", ok, d))
ok, d = same("B", 0x1401, "l21_melnik_repka_use")
checks.append(("Miller's door the host's scene opened opens on the joiner (3 s on)", ok, d))
ok, d = same("D", 0x1401, "l21_melnik_repka_use")
checks.append(("... and goes on alike (7 s on)", ok, d))
x, y = hs.get("C2", {}).get(0x8900), cs.get("C2", {}).get(0x8900)
checks.append(("the hall door both started at once plays alike on both (no jump back on the joiner)",
               bool(x and x[0] == "l_00_open" and alike(x, y)), "host %s / joiner %s" % (x, y)))
x, y = hs.get("E", {}).get(0x1703), cs.get("E", {}).get(0x1703)
checks.append(("a joined game's own start is not sent to the host", bool(y and y[0] == "idle_open" and (not x or x[0] != "idle_open")),
               "host %s / joiner %s" % (x, y)))
sent = [int(m.group(1)) for l in host.log_lines() for m in [re.search(r"\(sent (\d+), played", l)] if m]
checks.append(("the host sent a sane number of notes", bool(sent) and sent[-1] < 400, "sent %s" % sent[-1:]))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
rx = re.compile(r"CRASH|stack:|motions: |lab: motion of|lab: play|asks for")
for x in (host, client):
    print("---- %s" % x.name)
    for l in [l for l in x.log_lines() if rx.search(l)][-30:]:
        print(l[1:13] + l[19:230])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
