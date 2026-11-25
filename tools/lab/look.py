"""look.py [--solo] [--save name] [--level level] [--seconds N] [--out name] [--gain G] [--gamma G]
           [--host steps] [--client steps] [--show regex]

What the two players see of each other: a co-op run of two test copies (as smoke.py) with pictures of the
frames each game draws ("!shot", read from the swap chain's back buffer - the windows stay off the screen).

The pictures go to re/shots/<out>/ as .png, with a contact sheet per side (sheet_host.png, sheet_client.png).
--gain brightens them (Metro is dark; default 1.6); --gamma 0.45 lifts the dark parts of a tunnel level.

Without --host / --client the run is the standard one, counted from the moment the joiner's body arrives:
  host:   picture, walks forward 2.5 s, picture, then turns right in 8 steps with a picture after each
  client: picture, picture after the host's walk, then the same turn in 8 steps
--host / --client replace those steps (" | " between steps, as in [lab] script; they run after "?pid==").

--solo: one copy only (half the memory). The host gives player 1 a body though nobody joined ("!ghost" at
--spawn-at seconds, default 16) and looks at it: the body stands where it was made.

Checks: both games ended by themselves, no crash, every picture asked for was written.
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


solo = "--solo" in a
spawn_at = float(opt("--spawn-at", "16"))
client_before = opt("--client-before", "")  # the joiner's steps run before it joins (absolute times below 20)
before = opt("--before", "")  # --solo: steps run before the body is made
level = opt("--level")
save = opt("--save", "hunter_start")
seconds = int(opt("--seconds", "60" if solo else "110"))
out_name = opt("--out", "look")
gain = float(opt("--gain", "1.6"))
gamma = float(opt("--gamma", "1"))
host_steps, client_steps = opt("--host"), opt("--client")
copy_name = opt("--copy", "test-m33")  # --solo: which test copy runs (another one while a sweep uses the first two)
show = opt("--show")


def turn(prefix, steps=8, ms=400):
    return " | ".join("+0.1: !pad 0 0 0 %d 32000 0 | +%.1f: !shot %s%d" % (ms, ms / 1000.0 + 0.4, prefix, i + 1)
                      for i in range(steps))


if host_steps is None and solo:
    host_steps = "+2: !shot h0_start | +0.5: !pad 0 0 32000 2500 | +3: !shot h1_walked | " + turn("h2_turn", 10)
if host_steps is None:
    host_steps = "+3: !shot h0_start | +0.5: !pad 0 0 32000 2500 | +3: !shot h1_walked | " + turn("h2_turn")
if client_steps is None:
    client_steps = "+3: !shot c0_start | +4: !shot c1_host_walked | " + turn("c2_turn")

require_memory(1 if solo else 2)
host, client = Copy(copy_name), Copy("test-m33b")
copies = (host,) if solo else (host, client)

if level:
    begin = r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s" % level
else:
    src = os.path.join(REPO, "re", "lab-saves", "2033_" + save)
    dst_dir = os.path.join(host.user, "4A Games", "Metro 2033")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, "2033_lab_start"))
    begin = "0.3: >stop_video | ?loader_finish: mapstate_running | +2.5: gameload lab_start"

if solo:
    host_script = begin + ((" | " + before) if before else "") + " | %g: !ghost | " % spawn_at + host_steps + " | +1: !net"
    client_script = ""
else:
    host_script = begin + ((" | " + before) if before else "") + " | ?pid== | " + host_steps + " | +1: !net"
    client_script = ("0.3: >stop_video | " + ((client_before + " | ") if client_before else "") + "20: !finish | ?pid== | "
                     + client_steps + " | +1: !net")

procs = [host.start(seconds, host_script, extra="[net]\nmode = host", bind_port=None if copy_name == "test-m33" else 1003)]
if not solo:
    procs.append(client.start(seconds, client_script, bind_port=1001, autostart=False,
                              extra="[net]\nmode = join\naddress = 127.0.0.1"))
codes = wait(procs, seconds + 240)

out_dir = os.path.join(REPO, "re", "shots", out_name)
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
asked = lambda script: re.findall(r"!shot (\w+)", script)
got = {}
for c, script, side in ((host, host_script, "host"), (client, client_script, "client"))[:len(copies)]:
    files = shots(c, out_dir, gain, gamma)
    got[side] = [os.path.splitext(os.path.basename(f))[0] for f in files]
    if files:
        print("%s: %d pictures, sheet %s" % (side, len(files), sheet(files, os.path.join(out_dir, "sheet_%s.png" % side))))

has = lambda copy, text: any(text in l for l in copy.log_lines())
checks = [("both games ended by themselves", codes == [0] * len(copies), str(codes))]
crashes = [c.name for c in copies if has(c, "CRASH ")]
checks.append(("no crash", not crashes, ", ".join(crashes)))
missing = [n for side, script in (("host", host_script), ("client", client_script))[:len(copies)]
           for n in asked(script) if n not in got[side]]
checks.append(("every picture was written", not missing, ", ".join(missing)))

failed = 0
for n, (what, ok, detail) in enumerate(checks, 1):
    print("%2d  %-4s %s%s" % (n, "ok" if ok else "FAIL", what, ("   [" + detail + "]") if detail else ""))
    failed += not ok
if failed or show:
    rx = re.compile(show or r"session:|net \[|net:|\[net\]|CRASH|  r[a-z0-9]+ |stack:|console: quit|pid==|lab: ")
    for c in copies:
        lines = [l for l in c.log_lines() if rx.search(l)]
        print("---- %s" % c.name)
        for l in (lines if show else lines[-14:]):
            print(l[1:13] + l[19:260])
print("pictures: %s" % out_dir)
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
