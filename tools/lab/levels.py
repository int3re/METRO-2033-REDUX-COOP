"""levels.py [--only l01_hunter,l04_riga] [--skip l00_intro] [--seconds N] [--out file]

The join test on every story level: for each level the host starts it from its beginning ("map"), a second copy
joins, gets its body, and both run for a while (smoke.py --no-pad: no walking - many levels start in a scene
where the player cannot move). One line per level goes to the screen and to re/levels.txt (or --out); a level
that is already in that file with "ok" is skipped, so a stopped sweep goes on where it was.

About three minutes a level. (l05_pogruzchik and l10_bump are folders of the game, not levels: "map" with
such a name falls inside the game itself, exe+2958B - a script file that is not there.)
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from lab import LEVELS  # noqa: E402

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


only = opt("--only")
skip = (opt("--skip") or "").split(",")
seconds = opt("--seconds", "130")
out_path = opt("--out", os.path.join(REPO, "re", "levels.txt"))
levels = only.split(",") if only else [l for l in LEVELS if l not in skip]

done = {}
if os.path.exists(out_path):
    for line in open(out_path, encoding="utf-8"):
        m = re.match(r"(\S+)\s+(ok|FAIL)\b", line)
        if m:
            done[m.group(1)] = m.group(2)

for level in levels:
    if done.get(level) == "ok":
        print("%-20s ok   (earlier)" % level, flush=True)
        continue
    t = time.time()
    p = subprocess.run([sys.executable, os.path.join(HERE, "smoke.py"), "--level", "2033\\" + level, "--no-pad", "--eps", "0.5", "--seconds",
                        seconds, "--show", r"CRASH|stack:|cannot host|could not be hosted"], capture_output=True, text=True,
                       errors="replace")
    text = p.stdout + p.stderr
    result = next((l for l in reversed(text.splitlines()) if l.startswith("RESULT:")), "RESULT: none")
    fails = [l.strip() for l in text.splitlines() if re.match(r"\s*\d+\s+FAIL", l)]
    crash = [l.strip()[:150] for l in text.splitlines() if "CRASH" in l or "stack:" in l]
    ok = p.returncode == 0 and result.startswith("RESULT: PASS")
    line = "%-20s %-4s %s  (%.0f s)%s%s" % (level, "ok" if ok else "FAIL", result[8:], time.time() - t,
                                           ("  | " + " | ".join(fails)) if fails else "", ("  | " + " | ".join(crash[:4])) if crash else "")
    print(line, flush=True)
    with open(out_path, "a", encoding="utf-8") as f:
        f.write(line + "\n")
