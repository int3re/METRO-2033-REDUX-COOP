"""drive.py [--level level] [--target id] [--door id] [--hold ms] [--seconds N] [--show regex]

A wheel one player holds and turns (l00_intro: the prologue's gate wheel, entity 6E00): the joined copy uses it
("!useid") and holds Use ("!holdact 35", the game's input action as the level's script hears it). Its PC gives the
wheel's "motion control" commands, tells them to the host, and the host gives the same commands to its own copy:
the gate the wheel lifts (zatvor_0002, entity 400, moved by that block) goes as far on both. While the joined player is at the wheel the host's player may not use it ("only
one turns"), and the host's player is not moved.

Checks:
  1  both games ended by themselves
  2  no crash
  3  the joined game's hold drove the wheel's motion control there (its commands told to the host)
  4  the host gave the same commands to its own copy
  5  the host's player could not use the wheel while the joined player was at it
  6  the host's gate moved while the joined player held Use
  7  the gate stands alike on both PCs at the end (the same motion, less than 0.5 s apart)
  8  the host's player stayed where it stood
"""
import os, re, sys
from lab import Copy, wait, wait_hosting, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level", r"2033\l00_intro")
target = opt("--target", "6E00")
door = opt("--door", "400")
hold = int(opt("--hold", "9000"))
seconds = int(opt("--seconds", "130"))
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
polls = "".join(" | +2: !motionof %s | +0.1: !motionof %s | +0.1: !mayuse %s" % (target, door, target) for _ in range(30))
ph = host.start(seconds, begin + " | ?pid== | +1: !useon %s | +0.5: !motions 100 | +0.5: !net" % target + polls + " | +1: !net",
                extra="[net]\nmode = host")
late = wait_hosting(host, ph)
steps = " | +1.5: !motionof %s | +0.1: !motionof %s" % (target, door)
pc = client.start(max(60, int(round(seconds - late))), "0.3: >stop_video | 8: !finish | ?pid== | +0.5: !motions 60 | +0.5: !useon %s | +3: !useid %s"
                  " | +0.3: !holdact 35 %d" % (target, target, hold) + steps * int(hold / 1500 + 4) + " | +2: !net",
                  bind_port=1001, autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

STATE = re.compile(r"net \[lab\]: network game \d+, host \w+, local slot (-?\d+), players (\d+), level \w+, body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)")
MOTION = re.compile(r"lab: motion of %s '[^']*': '([^']*)' \(#(\d+)\)[^,]*, [\d.]+ s ago, time ([-\d.]+) of ([-\d.]+)" % door)
places = []
for l in host.log_lines():
    m = STATE.search(l)
    if m and m.group(1) == "0":
        places.append(tuple(float(x) for x in m.group(4, 5, 6)))
moved = ((places[0][0] - places[-1][0]) ** 2 + (places[0][2] - places[-1][2]) ** 2) ** 0.5 if len(places) >= 2 else -1.0


def motions_of(c):
    return [(m.group(1), int(m.group(2)), float(m.group(3)), float(m.group(4))) for m in (MOTION.search(l) for l in c.log_lines()) if m]


hm, cm = motions_of(host), motions_of(client)
has = lambda c, t: any(t in l for l in c.log_lines())
times = [t for _, _, t, _ in hm]
went_on = len(times) >= 2 and max(times) - min(times) > 0.3
alike = bool(hm and cm) and hm[-1][1] == cm[-1][1] and abs(hm[-1][2] - cm[-1][2]) < 0.5
checks = [
    ("both games ended by themselves", codes == [0, 0], str(codes)),
    ("no crash", not any(has(c, "CRASH ") for c in (host, client)), ""),
    ("the joined game's hold drove the wheel's motion control (told to the host)", has(client, "drive: this player's 'fwd'"), ""),
    ("the host gave the same commands to its own copy", has(host, "drive: player 1's 'fwd'") and has(host, "given here too"), ""),
    ("the host's player could not use the wheel while the joined player was at it", has(host, "may this player use %s now: no" % target), ""),
    ("the host's gate moved while the joined player held Use", went_on, "host times: %s" % ", ".join("%.2f" % t for t in times[:14])),
    ("the gate stands alike on both PCs at the end", alike, "host %s / joiner %s" % (hm[-1] if hm else None, cm[-1] if cm else None)),
    ("the host's player stayed where it stood", 0 <= moved < 0.3, "%.2f m" % moved),
]
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
print("info: fwd_end on the host: %s" % has(host, "'fwd_end' of a thing player 1 turns came here too"))
rx = re.compile(show or r"drive:|lab: motion|lab: input|may this|CRASH|stack:|used a thing|scene block|plays here too")
for c in (host, client):
    print("---- %s" % c.name)
    for l in [l for l in c.log_lines() if rx.search(l)][-40:]:
        print(l[1:13] + l[19:260])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
