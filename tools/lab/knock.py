"""knock.py [--delay N] [--seconds N] [--level level] [--show regex]

A joiner that comes too early: the joining copy is started FIRST and knocks at a host that is not there; the host
copy is started --delay seconds later (default 35) and loads its level. The joined game must keep knocking and
get in once the host takes connections (before 0.1.7 it knocked once, waited a minute and gave up for good).

Checks:
  1  both games ended by themselves
  2  no crash
  3  the joiner knocked more than once
  4  the host hosted its level and a player joined
  5  the joiner loaded the host's level and has its body on both PCs
"""
import os, re, shutil, sys, time
from lab import Copy, wait, require_memory, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


delay = int(opt("--delay", "35"))
seconds = int(opt("--seconds", "150"))
level = opt("--level")
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    src = os.path.join(REPO, "re", "lab-saves", "2033_hunter_start")
    dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))
    # (not "4: gameload": with the other copy already running the menu may come up a moment later than the 4th
    # second, and a "gameload" said before the menu runs is lost)
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

# the joiner first; its "press a key" of the host's level is answered by the mod itself (finish_load), the lab's
# "!finish" only gets it through its own start-up
pc = client.start(seconds, "0.3: >stop_video | 8: !finish | ?pid== | +6: !net | %d: !net" % (seconds - 6), bind_port=1001,
                  autostart=False, extra="[net]\nmode = join\naddress = 127.0.0.1")
time.sleep(delay)
ph = host.start(seconds - delay, begin + " | ?pid== | +6: !net | %d: !net" % (seconds - delay - 6), extra="[net]\nmode = host")
codes = wait([ph, pc], seconds + 240)


def has(copy, text):
    return any(text in l for l in copy.log_lines())


pid = lambda copy: next((re.search(r"pid==1, eid==(\d+)", l).group(1) for l in reversed(copy.log_lines())
                         if "pid==1, eid==" in l), None)
knocks = sum(1 for l in client.log_lines() if "console: connect " in l or "^connect " in l)
again = [l for l in client.log_lines() if "knocking again" in l]

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
crashes = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not crashes, ", ".join(crashes)))
checks.append(("the joiner knocked more than once", bool(again), "%d 'connect' lines, %d 'knocking again'" % (knocks, len(again))))
checks.append(("the host hosted its level and a player joined",
               has(host, "session: hosting the level") and has(host, "[net] incoming connection"), ""))
hp, cp = pid(host), pid(client)
checks.append(("the joiner loaded the host's level and has its body on both PCs", hp is not None and hp == cp, "%s / %s" % (hp, cp)))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
keep = re.compile(show or r"session:|\[net\]|CRASH|console: (connect|quit|disconnect)|\^connect|\^disconnect|loading content|mapstate|lab: (waiting|got)")
if failed or show:
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if keep.search(l)][-40:]:
            print(l[1:13] + l[19:230])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
