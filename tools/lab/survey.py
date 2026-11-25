"""survey.py level [--seconds N] [--copy test-m33c] [--steps "lab steps"]

One test copy (mode = host, nobody joins) on a level from its start: after the level runs it writes its world
("!world 0" -> <copy>/world-0.txt) and the level's use blocks with the entities they wait on ("!uses"), then the
given steps. Prints the use blocks and the world's entities grouped by name, so the things of a level - doors,
cabinets, boxes, wheels - can be found by name.

  python tools/lab/survey.py "2033\\l00_intro" --steps "+2: !near 30"
"""
import os, re, sys
from lab import Copy, wait, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


seconds = int(opt("--seconds", "75"))
name = opt("--copy", "test-m33c")
steps = opt("--steps", "")
level = a[0] if a else r"2033\l00_intro"

require_memory(1)
c = Copy(name)
world = os.path.join(c.dir, "world-0.txt")
if os.path.exists(world):
    os.remove(world)
script = (r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?loader_finish: mapstate_running | +6: !world 0 | +0.5: !uses" % level)
if steps:
    script += " | " + steps
p = c.start(seconds, script, bind_port=1003, extra="[net]\nmode = host")
code = wait([p], seconds + 180)[0]
lines = c.log_lines()
print("ended: %s; crash: %s" % (code, any("CRASH " in l for l in lines)))
print("---- use blocks")
for l in lines:
    if "net:   use block" in l or "use blocks," in l:
        print("  " + l[29:200])
if os.path.exists(world):
    groups = {}
    for row in open(world, encoding="latin1"):
        f = row.rstrip("\n").split("\t")
        if len(f) < 11:
            continue
        base = re.sub(r"_\d+$", "", f[10])
        groups.setdefault((f[1], base), []).append(f[0])
    print("---- %d kinds of entities (class rva, name without its number: count, first ids)" % len(groups))
    for (cls, base), ids in sorted(groups.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        print("  %-7s %-44s %4d  %s" % (cls, base[:44], len(ids), " ".join(ids[:6])))
if steps:
    print("---- the steps' lines")
    for l in lines:
        if "lab:" in l or "net:" in l:
            if "use block" not in l:
                print("  " + l[29:220])
