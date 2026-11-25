"""hostquit.py [--out name] [--how quit|menu|joiner]

What a joined game does when the host's game goes away while both play: the host copy closes its game ("quit",
as the player's "Exit the game" does) or goes to its main menu (--how menu), the joined copy goes on running and
takes pictures for two minutes: before, just after, while it finds its way back, after the pad's A.

Run it with METRONET_AUTO=1 for the player's way (mode = auto through the stand-in for Steam). The pictures go to
re/shots/<out>/ with a sheet; the joined copy's log lines about the session and any stall are printed.

Checks: the joined game did not fall; its main loop never stood still (the lab's hang detector says so in the
log); it came back to a RUNNING main menu ("loader_finish: mapstate_running" of the menu level after the host left).
"""
import os, re, shutil, sys
from lab import Copy, wait, require_memory, shots, sheet, REPO

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


out = opt("--out", "hostquit")
how = opt("--how", "quit")
press = "--no-press" not in a  # --no-press: the joined copy presses nothing after the host left (the mod must manage)

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
dst = os.path.join(host.user, "4A Games", "Metro 2033")
os.makedirs(dst, exist_ok=True)
shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_hunter_start"), os.path.join(dst, "2033_lab_start"))
begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"
# quit: the host closes its game; menu: the host takes "Exit to the main menu" (the game's own way);
# joiner: the joined player takes "Exit to the main menu" and the host plays on
leave = "quit" if how == "quit" else "!mainmenu"
host_steps = " | ?pid== | +12: !mark the host leaves | +0.1: " + leave if how != "joiner" else " | ?pid== | +12: !mark the host stays"
ph = host.start(150, begin + host_steps, extra="[net]\nmode = host")
button = opt("--button", "1000")  # the pad button pressed after the host left (hex: 1000 A, 10 START, 2000 B)
A = ("!pad %s 0 0 200" % button) if press else "!mark (no press)"
steps = ("?pid== | +10: !shot a_before" + (" | +1.9: !mainmenu | +2.1: !shot b_left_2s" if how == "joiner" else " | +4: !shot b_host_left_2s") + " | +10: !shot c_12s | +15: !shot d_27s | +20: !shot e_47s"
         " | +2: " + A + " | +4: !shot f_after_A | +20: !shot g_71s | +25: !shot h_96s | +2: " + A +
         " | +5: !shot i_after_A2 | +10: !net")
pc = client.start(260, "0.3: >stop_video | 20: !finish | " + steps, bind_port=1001, autostart=False, after=(host, ph),
                  extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], 420)

out_dir = os.path.join(REPO, "re", "shots", out)
shutil.rmtree(out_dir, ignore_errors=True)
files = shots(client, out_dir, gain=1.6)
if files:
    print("%d pictures, sheet %s" % (len(files), sheet(files, os.path.join(out_dir, "sheet.png"), columns=3, width=480)))
lines = client.log_lines()
left_at = None
for l in host.log_lines():
    if "lab: mark the host leaves" in l or "lab: mark the host stays" in l:
        left_at = l[1:13]
interesting = re.compile(r"session:|steam: |hud:|menu:|mapstate|net: .*pauses|net: finish|net: taking|\^change_map|\^disconnect|\^map |mp_game|CRASH|stack:|"
                         r"main loop|thread|PUSH PAUSE|POP PAUSE|lab: pad|net: network game flag")
print("---- the joined copy (the host left at %s)" % left_at)
for l in lines:
    if interesting.search(l) and (left_at is None or l[1:13] >= left_at):
        print("   " + l[:200])
crash = any("CRASH " in l for l in lines)
# (a long level load of the join itself may stand the loop for 8 s on a lab copy: only after the host left counts)
stall = any(("has not turned" in l or "main loop stood" in l) and (left_at is None or l[1:13] >= left_at) for l in lines)
menu_running = left_at is not None and any("loader_finish: mapstate_running" in l and l[1:13] > left_at for l in lines)
# the main menu itself, not the blurred background of its level with nothing to press (2026-11-25): the
# menu's desk is a sharp picture (edges about 15-16), the background alone about 8
def sharpness(path):
    from PIL import Image, ImageFilter, ImageStat
    return ImageStat.Stat(Image.open(path).convert("L").filter(ImageFilter.FIND_EDGES)).mean[0]


last = sharpness(files[-1]) if files else 0.0
checks = [("the joined game did not fall", not crash and codes[1] == 0, codes),
          ("its main loop never stood still", not stall, ""),
          ("it is back in a running main menu", menu_running, ""),
          ("the main menu itself is on the screen (not a blurred background)", last > 12.0, "sharpness %.1f" % last)
          if how != "joiner" else
          # the joined player left on its own while the host plays on: the menu comes up and asks again whether to
          # join (the game's own window - the desk behind it blurred)
          ("the main menu asks again whether to join the host's game", any("menu: asked" in l and l[1:13] > left_at for l in lines), "")]
for i, (what, ok, extra) in enumerate(checks, 1):
    print("%2d  %-4s %s   %s" % (i, "ok" if ok else "FAIL", what, extra))
bad = sum(1 for c in checks if not c[1])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not bad else "FAIL (%d of %d)" % (bad, len(checks))))
sys.exit(1 if bad else 0)
