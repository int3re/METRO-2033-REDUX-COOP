"""use.py [--host-block N] [--client-block N] [--seconds N] [--show regex]

"Use" (a door, a lamp, a talk) reaches every PC. On l01_hunter (the lab's save) the host fires one of the level's
use blocks as if its player had used the thing ("!use n"), then the joined game fires another; each PC lists its
use blocks first ("!uses").

Checks:
  1  both games ended by themselves
  2  no crash
  3  both PCs have the same list of use blocks (count, and the things of each)
  4  the host's use was fired on the joined PC
  5  the joiner's use was fired on the host's PC
  6  the used things are in the same state on both PCs afterwards (place and flags, from "!world")
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


host_block = int(opt("--host-block", "40"))      # door_use_0003
client_block = int(opt("--client-block", "38"))  # door_use_0001
seconds = int(opt("--seconds", "115"))
show = opt("--show")
os.environ["METRONET_NET"] = (os.environ.get("METRONET_NET", "") + ";use = 1").strip(";")

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
for c in (host, client):
    for f in os.listdir(c.dir) if os.path.isdir(c.dir) else []:
        if re.match(r"world-\d+\.txt$", f):
            os.remove(os.path.join(c.dir, f))
dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
os.makedirs(dst_dir, exist_ok=True)
shutil.copy2(os.path.join(REPO, "re", "lab-saves", "2033_hunter_start"), os.path.join(dst_dir, "2033_lab_start"))

ph = host.start(seconds, "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start | ?pid== | +3: !uses | +3: !mark use | +0.1: !use %X | +16: !world 0 | +1: !net"
                % host_block, extra="[net]\nmode = host")
pc = client.start(seconds, "0.3: >stop_video | 20: !finish | ?pid== | +3: !uses | +10: !mark use | +0.1: !use %X | +9: !world 0 | +1: !net"
                  % client_block, bind_port=1001, autostart=False, after=(host, ph), extra="[net]\nmode = join\naddress = 127.0.0.1")
codes = wait([ph, pc], seconds + 240)

BLOCK = re.compile(r"net:   use block (\d+) \((on|off)\):(.*)")


def blocks(copy):
    out = {}
    for l in copy.log_lines():
        m = BLOCK.search(l)
        if m:
            out[int(m.group(1))] = (m.group(2), re.sub(r"\s+", " ", m.group(3)).strip())
    return out


def world(copy):
    out = {}
    p = os.path.join(copy.dir, "world-0.txt")
    if os.path.exists(p):
        for l in open(p, encoding="latin1"):
            f = l.rstrip("\n").split("\t")
            if len(f) >= 11:
                out[int(f[0], 16)] = (f[2], tuple(float(x) for x in f[4:9]), f[10])
    return out


has = lambda copy, text: any(text in l for l in copy.log_lines())
hb, cb = blocks(host), blocks(client)
same = bool(hb) and hb.keys() == cb.keys() and all(hb[i][1] == cb[i][1] for i in hb)
diff = [i for i in sorted(set(hb) | set(cb)) if hb.get(i, ("", ""))[1] != cb.get(i, ("", ""))[1]]
things = []
for i in (host_block, client_block):
    for m in re.finditer(r"([0-9A-F]+) '", hb.get(i, ("", ""))[1]):
        things.append(int(m.group(1), 16))
hw, cw = world(host), world(client)
states = [(t, hw.get(t), cw.get(t)) for t in things]
# (flag bits 0x2000 / 0x4000 / 0x8000 are each PC's own "asleep" marks; a thing that is gone on both PCs agrees too)
same_state = lambda x, y: (x is None and y is None) or (x and y and (int(x[0], 16) & ~0xE000) == (int(y[0], 16) & ~0xE000)
                                                        and all(abs(p - q) < 0.05 for p, q in zip(x[1], y[1])))
agree = bool(states) and all(same_state(x, y) for _, x, y in states)

checks = []
checks.append(("both games ended by themselves", codes == [0, 0], str(codes)))
bad = [c.name for c in (host, client) if has(c, "CRASH ")]
checks.append(("no crash", not bad, ", ".join(bad)))
checks.append(("both PCs have the same list of use blocks", same, "host %d, joiner %d, differ at %s" % (len(hb), len(cb), diff[:8])))
checks.append(("the host's use was fired on the joined PC", has(client, "player 0 used a thing (use block %d" % host_block) and has(client, "fired here too"),
               hb.get(host_block, ("", "?"))[1]))
checks.append(("the joiner's use was fired on the host's PC", has(host, "player 1 used a thing (use block %d" % client_block) and has(host, "fired here too"),
               hb.get(client_block, ("", "?"))[1]))
checks.append(("the used things are in the same state on both PCs", agree,
               "; ".join("%X: host %s / joiner %s" % (t, x[:2] if x else None, y[:2] if y else None) for t, x, y in states)[:400]))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"CRASH|  r[a-z0-9]+ |stack:|session:|did not quit|pid==|used|lab fires|use blocks,|lab: mark")
    for c in (host, client):
        print("---- %s" % c.name)
        for l in [l for l in c.log_lines() if rx.search(l)][-16:]:
            print(l[1:13] + l[19:230])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
