"""suite.py [names...]

Every co-op test in a row (two test copies each, 2-3 minutes a test), with one line per test at the end.
Without names all of them run; with names only those.

  smoke      join, own body, both walk (l01_hunter from a save)
  armed      the same on l19_defence, both pull the trigger
  nextlevel  the host goes on to the next level, the joiner follows and gets a new body
  leave      the joiner quits and joins again
  death      the joiner's health goes to zero: the host gives it a new body
  reload     the host loads a save with the joiner in the game
  guns       the joiner's weapons: slots, shots, reloads, the host's third slot
  ride       l03_chase: the joiner's handcar goes with the host's, the joiner sits beside the host
  monster    l19_defence: the joiner's bullets kill a nosalis (the host woke it)
  hostdeath  l03_chase: a nosalis kills the host, "load the last checkpoint" takes the joiner along
  three      a host and two joined games
  aside      the host has walked before the joiner comes: the joiner loads the level and stands clear of the host
  knock      the joiner is started before the host: it keeps knocking and gets in once the host's level runs

The exit code is 0 only when every test passed.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
TRIGGER = "+1: !pad 0 0 0 1500 0 0 0 255"
TESTS = [
    # name, script, arguments, texts that must be in its output
    ("smoke", "smoke.py", [], []),
    ("armed", "smoke.py", ["--level", r"2033\l19_defence", "--host-then", TRIGGER, "--client-then", TRIGGER], []),
    ("nextlevel", "smoke.py", ["--seconds", "170", "--host-then",
                               r"+2: save_player 2033\l02_exhibition | +0.5: change_map 2033\l02_exhibition",
                               "--show", "following the host|goes on in the new level"],
     ["following the host", "goes on in the new level"]),
    ("leave", "leave.py", [], []),
    ("death", "smoke.py", ["--seconds", "130", "--client-then", "+2: !health FFFF 0", "--show", "player 1 died|a body for slot"],
     ["player 1 died"]),
    ("reload", "smoke.py", ["--seconds", "150", "--no-pad", "--host-then", "+2: gameload lab_start", "--show", "told to load|loads a save"],
     ["told to load"]),
    ("guns", "guns.py", [], []),
    ("ride", "ride.py", [], []),
    ("monster", "monster.py", [], []),
    ("hostdeath", "hostdeath.py", [], []),
    ("three", "three.py", [], []),
    ("aside", "aside.py", [], []),
    ("knock", "knock.py", [], []),
]

want = sys.argv[1:]
rows = []
for name, script, args, must in TESTS:
    if want and name not in want:
        continue
    t = time.time()
    p = subprocess.run([sys.executable, os.path.join(HERE, script)] + args, capture_output=True, text=True, errors="replace")
    out = p.stdout + p.stderr
    result = next((l for l in reversed(out.splitlines()) if l.startswith("RESULT:")), "RESULT: none (%s)" % out.strip()[-200:])
    missing = [m for m in must if m not in out]
    ok = p.returncode == 0 and result.startswith("RESULT: PASS") and not missing
    rows.append((name, ok, result[8:] + (("; not seen: " + ", ".join(missing)) if missing else ""), time.time() - t))
    print("%-10s %-4s %s  (%.0f s)" % (name, "ok" if ok else "FAIL", rows[-1][2], rows[-1][3]), flush=True)
    if not ok:
        print("\n".join("    " + l for l in out.splitlines()[-45:]), flush=True)
bad = [r[0] for r in rows if not r[1]]
print("SUITE: %s" % ("PASS (%d tests)" % len(rows) if not bad else "FAIL: " + ", ".join(bad)))
sys.exit(1 if bad else 0)
