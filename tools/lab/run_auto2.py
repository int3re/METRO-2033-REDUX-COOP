"""run_auto2.py [seconds] [level] [--delay N]

Two test copies in one network game, driven by the mod's own session logic ([net] mode = host / join), not by
script commands: the host's script only starts the level (what "new game" does), the client's script only
starts its main menu (what a key press on the title screen does). Prints the session lines of both logs.
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


delay = int(opt("--delay", "2"))
menu_at = float(opt("--menu-at", "18"))
seconds = int(a[0]) if a else 100
level = a[1] if len(a) > 1 else r"2033\l01_hunter"

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
ph = host.start(seconds, r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s | %d: !net" % (level, seconds - 6),
                extra="[net]\nmode = host\nbody_delay = %d" % delay)
pc = client.start(seconds, "0.3: >stop_video | %g: !finish | %d: !net" % (menu_at, seconds - 6), bind_port=1001,
                  autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1")
print("exit codes", wait([ph, pc], seconds + 240))

KEEP = re.compile(r"session:|net \[|net:|\[net\]|CRASH|  r[a-z0-9]+ |stack:|console: quit|pid==|player is absent|loader_finish|"
                  r"loading content|load save bin|relevant: player")
DROP = re.compile(r"socket port|answered as no player")
for c in (host, client):
    lines = [l for l in c.log_lines() if KEEP.search(l) and not DROP.search(l)]
    print("==== %s" % c.name)
    for l in lines:
        print(l[1:13] + l[19:300])
