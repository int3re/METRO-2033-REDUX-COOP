"""worldsweep.py [--only l01_hunter,l04_riga] [--times "60,100"] [--out file]

world.py on every story level: which things of the level stand in different places on the two PCs a minute or
two after the join (vehicles, lifts, doors, scene dummies the level's script moves on each PC by itself).
One block per level goes to re/world-levels.txt (or --out): the summary line and the things / characters that
are apart at the last time point. A level already in the file is skipped.

About three minutes a level.
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
times = opt("--times", "60,100")
out_path = opt("--out", os.path.join(REPO, "re", "world-levels.txt"))
levels = only.split(",") if only else LEVELS

done = set()
if os.path.exists(out_path):
    done = set(re.findall(r"^## (\S+)", open(out_path, encoding="utf-8").read(), re.M))

for level in levels:
    if level in done:
        print("%-20s (earlier)" % level, flush=True)
        continue
    t = time.time()
    p = subprocess.run([sys.executable, os.path.join(HERE, "world.py"), "--level", "2033\\" + level, "--times", times, "--list", "14"],
                       capture_output=True, text=True, errors="replace")
    text = p.stdout + p.stderr
    result = next((l for l in reversed(text.splitlines()) if l.startswith("RESULT:")), "RESULT: none")
    last = text.rfind("==== ")
    block = [l for l in (text[last:] if last >= 0 else "").splitlines()
             if re.match(r"\s+(actor apart|thing apart|only the|host \d+ entities|in different places|\.\.\. and)", l)
             and "ammo_" not in l]
    crash = [l.strip()[:160] for l in text.splitlines() if "CRASH" in l or "stack:" in l]
    print("%-20s %s  (%.0f s)" % (level, result[8:], time.time() - t), flush=True)
    with open(out_path, "a", encoding="utf-8") as f:
        f.write("## %s  %s\n" % (level, result[8:]))
        for l in block + crash:
            f.write(l.rstrip()[:260] + "\n")
        f.write("\n")
