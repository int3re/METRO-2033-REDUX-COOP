"""leave.py [--level level] [--stay N] [--again N]

A player leaves a session and comes back: the host runs all the time; the joiner's copy runs `stay` seconds
(joins, gets its body, walks, quits by itself), then it is started again (`again` seconds) and joins anew.

Checks:
  1  no crash on the host or in either run of the joiner
  2  the first joiner got a body
  3  after it left, the host took that body out of its world
  4  the second joiner got a body of its own (another id) and controls it
  5  the host and the second joiner agree on both positions at the end
  6  the host's save written after all that holds no joined player's body
"""
import os, re, shutil, sys, time
from lab import Copy, wait, require_memory, REPO, wait_hosting

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level")
# (A copy's life is counted in seconds, so the numbers leave room for a slow hour of the PC: a joiner needs up to
# 50 s to its body - its menu, the host's level, the body - and the host's own level loads up to 30 s before the
# first joiner is even started.)
stay = int(opt("--stay", "70"))
again = int(opt("--again", "85"))
total = stay + again + 85

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    dst = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst, exist_ok=True)
    shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_hunter_start"), os.path.join(dst, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

# The host: its state after the first join, after the leave, after the second join; a save at the end.
host_script = (begin + " | ?pid== | +4: !net | ?[net] disconnect | +6: !net | ?pid== | +12: !net"
               " | +1: gamesave | +7: !net")
join_script = "0.3: >stop_video | 20: !finish | ?pid== | +4: !net | +0.5: !pad 0 0 32000 2500 | +4: !net | %d: !net"

ph = host.start(total, host_script, extra="[net]\nmode = host")
wait_hosting(host, ph)
pa = client.start(stay, join_script % (stay - 4), bind_port=1001, autostart=False,
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
code_a = wait([pa], stay + 120)[0]
first = client.log_lines()
time.sleep(3)
pb = client.start(again, join_script % (again - 4), bind_port=1001, autostart=False,
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
code_b, code_h = wait([pb, ph], total + 240)
second, hl = client.log_lines(), host.log_lines()

STATE = re.compile(r"net \[lab\]: .*players (\d+), level \w+, body ([0-9A-F]+) at \(([-\d.]+),([-\d.]+),([-\d.]+)\)(.*)")
SLOT = re.compile(r" s(\d+)=([0-9A-F]+)/(\d)/(peer|-)\(([-\d.]+),([-\d.]+),([-\d.]+)\)")


def last_state(lines):
    for l in reversed(lines):
        m = STATE.search(l)
        if m:
            return dict(body=m.group(2), slots={int(s[0]): (s[1], int(s[2]), tuple(float(x) for x in s[4:7]))
                                                 for s in SLOT.findall(m.group(6))})
    return None


def pids(lines):
    return [(int(m.group(1)), int(m.group(2))) for m in (re.search(r"pid==(\d+), eid==(\d+)", l) for l in lines) if m]


def near(p, q, eps=0.15):
    return p is not None and q is not None and all(abs(x - y) <= eps for x, y in zip(p, q))


crashed = [n for n, ls in (("host", hl), ("joiner 1", first), ("joiner 2", second)) if any("CRASH " in l for l in ls)]
p1, p2 = pids(first), pids(second)
took = [l for l in hl if "session: the body of player" in l and "is gone" in l]
gone = [l for l in hl if "net: no entity" in l or "net: the world holds" in l]
hs, cs = last_state(hl), last_state(second)
slot2 = p2[-1][0] if p2 else None
left_out = [l for l in hl if "the host's save was written" in l]

checks = [
    ("no crash; all three ended by themselves", not crashed and [code_h, code_a, code_b] == [0, 0, 0],
     "%s %s" % (crashed, [code_h, code_a, code_b])),
    ("the first joiner got a body", bool(p1), str(p1[-1:] )),
    ("after it left the host took the body out", bool(took), took[-1][34:150] if took else ""),
    ("the second joiner got another body and controls it",
     bool(p2) and bool(p1) and p2[-1][1] != p1[-1][1] and bool(cs) and int(cs["body"], 16) == p2[-1][1], str(p2[-1:])),
    ("the host and the second joiner agree on both positions",
     bool(hs) and bool(cs) and slot2 in hs["slots"] and slot2 in cs["slots"] and near(hs["slots"][0][2], cs["slots"][0][2])
     and near(hs["slots"][slot2][2], cs["slots"][slot2][2]),
     "%s / %s" % (hs["slots"] if hs else None, cs["slots"] if cs else None)),
    ("the host's last save left the joined player's body out", bool(left_out) and "(0 entities" not in left_out[-1],
     left_out[-1][34:140] if left_out else ""),
]
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
keep = re.compile(r"session:|net: (taking|a body|entity|no entity|the world|the host|a checkpoint)|\[net\] (dis|inc|conn)|CRASH|stack:|  r[a-z0-9]+ |"
                  r"net \[lab|pid==|console: quit")
if failed or "--show" in a:
    for name, ls in (("host", hl), ("joiner 1", first), ("joiner 2", second)):
        print("---- %s" % name)
        for l in [l for l in ls if keep.search(l)][-26:]:
            print(l[1:13] + l[19:250])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
