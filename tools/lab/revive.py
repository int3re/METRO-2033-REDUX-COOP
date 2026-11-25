"""revive.py [--seconds N] [--hold-at S] [--show regex]

A player who would die falls instead, and a teammate raises them. The run starts from the save chase_fight
(l03_chase, the handcar after the vision: a nosalis jumps on about 30 s later and kills a player who sits still
in about 50 s - hostdeath.py). The host loads it, a second copy joins and sits beside the host on the handcar;
nobody fights. The nosalis brings the host down (no death menu: the health stops at 1 %); --hold-at seconds after
the joiner's body arrived the joiner holds "use" (the pad's X) for 4 s next to the host and raises them.

Checks:
  1  both games ended by themselves, no crash
  2  the host went down, not dead (its log: "player 0 is down"; no death menu before everybody was down)
  3  the joiner saw the host down (its log)
  4  the joiner raised the host ("this player raised player 0"; the host: "raised you" - its health back up)
  5  when everybody was down (if it came to it), the last checkpoint was loaded for everybody
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


seconds = int(opt("--seconds", "200"))
hold_at = float(opt("--hold-at", "75"))
all_down_run = "--all-down" in a  # the joiner does not raise: it goes down itself (its health set to 1 %) - everybody down
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
dst = os.path.join(host.user, "4A Games", "Metro 2033")
os.makedirs(dst, exist_ok=True)
shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_chase_fight"), os.path.join(dst, "2033_lab_start"))
N = "".join(" | +4: !net" for _ in range(int((seconds - 60) / 4)))
ph = host.start(seconds, "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"
                         " | ?loader_finish: mapstate_running | +5: gamesave auto_save | ?pid==" + N, extra="[net]\nmode = host")
joiner_steps = (" | +%g: !mark down too | +0.1: !health FFFF 1 | +30: !net" % hold_at if all_down_run else
                " | +%g: !mark hold | +0.1: !pad 4000 0 0 4000 | +6: !net | +20: !mark hold again | +0.1: !pad 4000 0 0 4000 | +6: !net" % hold_at)
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid==" + joiner_steps,
                  bind_port=1001, autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

H, C = host.log_lines(), client.log_lines()
has = lambda lines, text: [l for l in lines if text in l]
crash = [x.name for x in (host, client) if has(x.log_lines(), "CRASH ")]
host_down = has(H, "revive: player 0 is down")
all_down = has(H, "every player is down")
first_all = all_down[0][1:13] if all_down else "99"
menu_before = [l for l in has(H, "Activate menu [die_in_game]") if l[1:13] < first_all]
checks = [
    ("both games ended by themselves, no crash", codes == [0, 0] and not crash, "%s %s" % (codes, crash)),
    ("the host went down, not dead", bool(host_down) and not menu_before, "%d downs, %d death menus before all were down" % (len(host_down), len(menu_before))),
    ("the joiner saw the host down", bool(has(C, "revive: player 0 is down")), ""),
    ("the joiner raised the host", bool(has(C, "this player raised player 0")) and bool(has(H, "revive: player 0 is up again")), "")
    if not all_down_run else
    ("both were down: the host loaded the last checkpoint for everybody", bool(all_down) and bool(has(H, "gameload auto_save")), "%d times" % len(all_down)),
    ("everybody down: the checkpoint for everybody", not all_down or bool(has(H, "gameload auto_save")), "%d times" % len(all_down)),
]
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
rx = re.compile(show or r"revive:|hud:|die_in_game|gameload|CRASH|lab: mark")
for x in (host, client):
    print("---- %s" % x.name)
    for l in [l for l in x.log_lines() if rx.search(l)][-22:]:
        print(l[1:13] + l[19:200])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
