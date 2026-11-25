"""kill.py [--who guest|host] [--seconds N] [--show regex]

Death by real hits, on l19_defence: two test copies; one player walks a few metres off, the other aims at it
(the script's pad turns the body towards the entity, "!aim") and fires bursts until it is dead.

  --who guest (default): the host shoots the joiner. Expected: the joiner's health falls on both PCs, the host
                         sees it die and gives it a new body.
  --who host:            the joiner shoots the host. Expected: the host's health falls on both PCs; the host's
                         game goes through its own death (the last checkpoint is loaded) and the joiner is taken
                         along.

Checks (guest): both games ended by themselves / no crash / the shooter looked at the target / the target's
health fell on the host / ... on the joiner's PC / the host saw the joiner die / the joiner got a new body.
Checks (host):  ... / the host's game loaded its checkpoint / the joiner was told to load it too / the joiner
has a body again at the end.
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


who = opt("--who", "guest")
seconds = int(opt("--seconds", "180"))
show = opt("--show")
HOST_BODY, GUEST_BODY = "F107", "7D0A"

# both stand on one spot after the join: the joiner goes a metre off first, then the shooter aims (sideways and
# up / down) and fires bursts
walk = "+4: !move FFFF 225.5 -3.50 -8.5"  # (the joiner puts its own body there: its PC is the one that moves it)
many = " | +1: !net" * 30
if who == "guest":
    # (25 s of fire: the shooter first turns to the target - up to 3 s - and reloads in the middle; in 12 s it landed
    # five hits of the seven it takes, every other run)
    host_steps = "+7: !net | +0.2: !mark shoot | +0.1: !aim %s 25000 255 | +0.5: !net | +8: !net | +8: !net" % GUEST_BODY
    client_steps = walk + " | +3: !net" + many + " | +8: !net | +8: !net"
else:
    # (the joiner's bullets seldom find the host's body - see the notes. The joiner fires all the same; the deadly
    # hits are hit events of exactly the kind its bullets send, given to the host's game one a second: "!hit")
    host_steps = "+7: !net" + " | +0.5: !hit %s %s | +0.5: !net" % (GUEST_BODY, HOST_BODY) * 16 + " | +8: !net | +30: !net"
    client_steps = walk + " | +3: !net | +0.2: !mark shoot | +0.1: !aim %s 14000 255 | +0.5: !net | +8: !net | +30: !net" % HOST_BODY

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map 2033\l19_defence"
ph = host.start(seconds, begin + " | ?pid== | " + host_steps + " | +1: !net", extra="[net]\nmode = host\nfriendly_fire = 1")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== | " + client_steps + " | +1: !net", bind_port=1001,
                  autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1\nfriendly_fire = 1")
codes = wait([ph, pc], seconds + 240)

SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(?:peer|-)\([-\d.,]+\)h(-?[\d.]+)")


def health(copy, slot):
    """the health of a slot's body in every "!net" line of the copy, in order"""
    out = []
    for l in copy.log_lines():
        if "net [lab]" not in l:
            continue
        for s, actor, state, h in SLOT.findall(l):
            if int(s) == slot:
                out.append(float(h))
    return out


def has(copy, text):
    return any(text in l for l in copy.log_lines())


def aimed(copy):
    for l in copy.log_lines():
        m = re.search(r"lab: aimed at \w+ for \d+ ms: seen, .* on target (\d+) of (\d+) steps", l)
        if m:
            return int(m.group(1)), int(m.group(2))
    return 0, 0


slot = 1 if who == "guest" else 0
shooter = host if who == "guest" else client
hh, ch = health(host, slot), health(client, slot)
if who == "guest":  # the host's own record of the joiner's health is denser than the "!net" samples
    seen = [float(m.group(1)) for l in host.log_lines() for m in [re.search(r"session: player 1 health here [-\d.]+ -> ([-\d.]+)", l)] if m]
    hh = (hh[:1] or [1.0]) + seen
on, steps = aimed(shooter)
bodies = lambda c: [m.group(1) for l in c.log_lines() for m in [re.search(r"pid==1, eid==(\d+)", l)] if m]

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("the shooter looked at the target", steps > 0 and on * 3 >= steps, "%d of %d steps" % (on, steps)))
checks.append(("the target's health fell on the host", len(hh) >= 2 and min(hh[1:]) < hh[0] - 0.05, str(hh)))
if who == "guest":
    checks.append(("the target's health fell on the joiner's PC", len(ch) >= 2 and min(ch[1:]) < ch[0] - 0.05, str(ch)))
if who == "guest":
    checks.append(("the host saw the joiner die", has(host, "player 1 died"), ""))
    checks.append(("the joiner got a new body", len(set(bodies(client))) >= 2 and len(set(bodies(host))) >= 2,
                   "%s / %s" % (bodies(host), bodies(client))))
else:
    checks.append(("the host's game loaded its checkpoint", has(host, "the host loads a save"), ""))
    checks.append(("the joiner was told to load it too", has(host, "told to load"), ""))
    checks.append(("the joiner has a body again", len(bodies(client)) >= 2, str(bodies(client))))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|lab: aimed|lab: mark|net \[lab|loads a save|told to load|did not quit|pid==")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-30:]:
            print(l[1:13] + l[19:250])
if failed:  # the next test overwrites the logs: keep them
    import shutil, os
    from lab import REPO
    for c in (host, client):
        if os.path.exists(c.log):
            shutil.copy2(c.log, os.path.join(REPO, "re", "kill-%s-%s.log" % (who, c.name)))
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
