"""l36.py [runs] [--seconds N] [--keep dir]

The last level's fall of the joined game (l36_ostankino). About 100 s into the level the host's script takes a
wave of monsters out from under the rangers and Miller; the game's "client" brain then read the id of a monster
that was gone as the number of a motion and fell, about one run in four (fixed in 0.1.6: [net] missing_target,
1 by default; 0 brings the fall back). Runs the join test of that level `runs` times (default 6) and prints, for
every run, whether a game fell, whether the wave was really removed in it and how long the joined game lived
after that. The logs of a run that fell or failed are kept in re/l36-runs (or --keep).

Extra [net] lines for both copies come from METRONET_NET as in every lab run, e.g. METRONET_NET="missing_target = 0".
"""
import os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from lab import Copy  # noqa: E402

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


# Eight cores a copy here, not the lab's usual four: in this level's long fight the game itself can lock up for
# good when its three dozen threads are squeezed onto four cores at a lowered priority (a particle being made
# holds its collision object and waits for the volume tree, a ray query holds the tree and waits for that object;
# the host stood still in five long runs of fourteen on four cores, in none of eight on eight).
os.environ.setdefault("METRONET_LAB", "cores = 8")
seconds = opt("--seconds", "200")  # the hordes are removed 75-100 s into the level, not at the same time every run
keep = opt("--keep", os.path.join(REPO, "re", "l36-runs"))
runs = int(a[0]) if a else 6
os.makedirs(keep, exist_ok=True)
SHOW = re.compile(r"CRASH|  thread |points |stack:|calls:|  r[a-z0-9]+ is |\[rdi|are destroyed|the host's word|worker threads")

def clock(line):
    """[hh:mm:ss.mmm] of a log line, in seconds"""
    m = re.match(r"\[(\d+):(\d+):(\d+)\.(\d+)\]", line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0 if m else None


def wave(lines):
    """How long the joined game lived after it was told the wave is gone (the first 'attackers_new' the host's
    word takes out there), in seconds; None - the run ended before the wave was removed."""
    gone = next((clock(l) for l in lines if "'attackers_new'" in l and "is taken out by" in l and "+68C8EF" in l), None)
    end = next((clock(l) for l in lines if "console: quit" in l or "CRASH " in l), None)
    return None if gone is None or end is None or end < gone else end - gone


fell = tried = hung = 0
for n in range(1, runs + 1):
    r = subprocess.run([sys.executable, os.path.join(HERE, "smoke.py"), "--level", "2033\\l36_ostankino", "--no-pad",
                        "--seconds", seconds, "--eps", "3"], capture_output=True, text=True, errors="replace")
    result = [l for l in r.stdout.splitlines() if l.startswith("RESULT")]
    crashed = []
    for c in (Copy("test-m33"), Copy("test-m33b")):
        lines = c.log_lines()
        if any("CRASH " in l for l in lines):
            crashed.append(c.name)
            stamp = time.strftime("%H%M%S")
            shutil.copy2(c.log, os.path.join(keep, "%s-%s.log" % (stamp, c.name)))
            at = next(i for i, l in enumerate(lines) if "CRASH " in l)
            print("  ---- %s fell:" % c.name)
            for l in [l for l in lines[max(0, at - 400):at + 60] if SHOW.search(l)][-34:]:
                print("  " + l[1:13] + l[19:250])
    fell += bool(crashed)
    hung += [c.name for c in (Copy("test-m33"), Copy("test-m33b")) if any("did not quit by itself" in l for l in c.log_lines())] != []
    after = wave(Copy("test-m33b").log_lines())
    tried += after is not None
    if not result or "FAIL" in result[0]:  # a run that did not pass without a fall: both logs are kept too
        stamp = time.strftime("%H%M%S")
        for c in (Copy("test-m33"), Copy("test-m33b")):
            if c.name not in crashed and os.path.exists(c.log):
                shutil.copy2(c.log, os.path.join(keep, "%s-%s-failed.log" % (stamp, c.name)))
    if result and "FAIL" in result[0]:
        for l in r.stdout.splitlines():
            if re.match(r"\s*\d+\s+FAIL", l):
                print("  " + l.strip()[:230])
    print("run %d: %s%s   [%s]" % (n, result[0] if result else "no result", ("   FELL: " + ", ".join(crashed)) if crashed else "",
                                 "the wave was NOT removed in this run" if after is None else "the joined game lived %.1f s after the wave went" % after))
    sys.stdout.flush()
print("L36: %d of %d runs fell; a game stood still in %d; the wave was removed in %d" % (fell, runs, hung, tried))
sys.exit(1 if fell else 0)
