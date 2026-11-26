"""askload.py [--level level] [--seconds N]

"Load the last checkpoint" in a joined player's pause menu (the menu gives the console "gameload ..."): before 0.4.6
nothing happened - a joined game cannot load by itself. Now its PC asks the host, and the host loads the checkpoint
for everybody, as its own choice would.

Checks: both ended, no crash; the joined game asked the host; the host loaded (its "gameload" with the joined
bodies taken out first); the joined game came back into the host's level afterwards (a new body).
"""
import sys
from lab import Copy, wait, wait_hosting, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


level = opt("--level", r"2033\l01_hunter")
seconds = int(opt("--seconds", "150"))

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
ph = host.start(seconds, r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?pid== | +3: >save_player | +60: !net" % level,
                extra="[net]\nmode = host")
late = wait_hosting(host, ph)
pc = client.start(max(80, int(round(seconds - late))), "0.3: >stop_video | 8: !finish | ?pid== | +8: >gameload auto_save | +40: !net",
                  bind_port=1001, autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)
has = lambda c, t: sum(t in l for l in c.log_lines())
bodies = has(client, "the own body")
checks = [
    ("both games ended by themselves", codes == [0, 0], str(codes)),
    ("no crash", not any(has(c, "CRASH ") for c in (host, client)), ""),
    ("the joined game asked the host for the checkpoint", has(client, "the host is asked to load the last checkpoint") > 0, ""),
    ("the host loaded it for everybody", has(host, "asks for the last checkpoint: loaded for everybody") > 0 and has(host, "the host loads a save") > 0, ""),
    ("the joined game came back with a new body", has(client, "the own body") >= 2, "%d bodies" % bodies),
]
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
for c in (host, client):
    print("---- %s" % c.name)
    for l in [l for l in c.log_lines() if "checkpoint" in l or "loads a save" in l or "own body" in l or "CRASH" in l or "gameload" in l][-12:]:
        print(l[1:13] + l[19:220])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
