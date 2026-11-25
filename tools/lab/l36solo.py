"""l36solo.py [runs] [--mode off|host] [--seconds N]

Does the game stand still in the long fight of l36_ostankino without a second player? In co-op lab runs the HOST
sometimes locks up for good in that fight (a deadlock inside the game: a particle being made holds its collision
object and waits for the volume tree, a ray query holds the tree and waits for the object). This runs ONE copy on
that level - with the mod off (--mode off, the game as it is) or hosting with nobody joined (--mode host) - for
--seconds (default 200), `runs` times (default 6), and says for every run whether the game stood still.
The player's health is topped up every few seconds so that the fight goes on (a dead player's menu pauses the game).

Eight cores a copy, as tools/lab/l36.py runs it.
"""
import os, re, sys
os.environ.setdefault("METRONET_LAB", "cores = 8")
from lab import Copy, require_memory  # noqa: E402

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


mode = opt("--mode", "off")
seconds = int(opt("--seconds", "200"))
runs = int(a[0]) if a else 6

heal = " | +4: !health FFFF 64" * int((seconds - 40) / 4)
script = (r"0.3: >stop_video | +0.5: disconnect | +0.5: map 2033\l36_ostankino | ?^map 2033 | ?loader_finish: mapstate_running"
          r" | +2: !unpause" + heal)
stood = 0
for n in range(1, runs + 1):
    require_memory(1)
    c = Copy("test-m33")
    code = c.run(seconds, script, extra="[net]\nmode = %s" % mode)
    lines = c.log_lines()
    hung = any("did not quit by itself" in l for l in lines) or any("has not turned for 8 s" in l for l in lines[len(lines) // 3:])
    died = sum(1 for l in lines if "Activate menu [die_in_game]" in l)
    crash = any("CRASH " in l for l in lines)
    last = next((l[1:13] for l in reversed(lines) if "[game]" in l and "quit" not in l), "?")
    stood += hung
    print("run %d (%s): exit %s%s%s, the death menu opened %d times, the game's last own line at %s"
          % (n, mode, code, "   STOOD STILL" if hung else "", "   FELL" if crash else "", died, last), flush=True)
    if hung or crash:
        keep = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "re", "l36-runs")
        os.makedirs(keep, exist_ok=True)
        import shutil, time
        shutil.copy2(c.log, os.path.join(keep, "%s-solo-%s.log" % (time.strftime("%H%M%S"), mode)))
print("L36 SOLO (%s): the game stood still in %d of %d runs" % (mode, stood, runs))
