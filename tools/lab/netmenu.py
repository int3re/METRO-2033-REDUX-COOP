"""netmenu.py [--seconds N] [--show regex]

The main menu's "СЕТЕВАЯ ИГРА" from end to end, the way a player gets it (mode = auto, through the lab's stand-in
for Steam): one copy plays the story (its Steam lobby is open), the other stands at the main menu's desk, opens
the network page (the label's click / N: "!netpage"), waits until the friend's game is listed on it, presses
Enter ("Войти": "!netenter") and comes into that game with a body of its own.

Checks:
  1  both games ended by themselves
  2  no crash
  3  the page opened on the desk
  4  the friend's game was listed on it
  5  Enter went into that game (the session joins the chosen lobby)
  6  the joiner got its body in the friend's game
Pictures of the joiner's page (before and after the friend is listed) and of the game it came into: re/shots/netmenu.
"""
import os, re, sys

os.environ["METRONET_AUTO"] = "1"  # (before lab is read: every run as a player gets it, with the stand-in for Steam)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shutil  # noqa: E402
from lab import Copy, wait, wait_hosting, require_memory, shots, sheet, REPO  # noqa: E402

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


seconds = int(opt("--seconds", "120"))
pad = "--pad" in a  # the pad's own buttons instead of the lab's commands: Y on the desk, down to the row, A
show = opt("--show")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
src = os.path.join(REPO, "re", "lab-saves", "2033_hunter_start")
dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
os.makedirs(dst_dir, exist_ok=True)
shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))

host_script = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start | ?pid== | +6: !net"
ph = host.start(seconds, host_script, extra="[net]\nmode = host")
late = wait_hosting(host, ph)
# (its ini says "mode = auto" itself: the lab answers the game's own "join?" window only for a "mode = join" copy, and
# the page must be what takes it there; the page opens before that window would come)
client_script = ("0.3: >stop_video | ?loader: mapstate_loaded_and_waiting | +3: !finish | ?loader_finish: mapstate_running"
                 " | +2: !pad 1000 0 0 300 | +0.8: %s | +2.5: !shot a_page"
                 " | +0.5: %s | ?pid== | +6: !shot c_joined | +1: !net") % (
                    ("!pad 8000 0 0 300", "!pad 2 0 0 250 | +0.6: !shot b_row | +0.3: !pad 1000 0 0 250") if pad else ("!netpage", "!netenter"))
pc = client.start(max(60, int(round(seconds - late))), client_script, bind_port=1001, autostart=False,
                  extra="[net]\nmode = auto")
codes = wait([ph, pc], seconds + 240)

out_dir = os.path.join(REPO, "re", "shots", "netmenu")
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
files = shots(client, out_dir)
if files:
    print("joiner: %d pictures, sheet %s" % (len(files), sheet(files, os.path.join(out_dir, "sheet.png"), columns=2, width=640)))

has = lambda c, t: any(t in l for l in c.log_lines())
checks = [
    ("both games ended by themselves", codes == [0, 0], str(codes)),
    ("no crash", not any(has(c, "CRASH ") for c in (host, client)), ""),
    ("the page opened on the desk", has(client, "netmenu: the network page opens"), ""),
    ("the friend's game was listed on it", has(client, "network page lists 1 friends"), ""),
    ("Enter went into that game", has(client, "netmenu: going into") and has(client, "session: joining the friend's game"), ""),
    ("the joiner got its body there", has(client, "pid==") or has(client, "own body"), ""),
]
failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"netmenu|session:|steam:|CRASH|stack:|Activate menu|menu: ")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-25:]:
            print(l[1:13] + l[19:240])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
