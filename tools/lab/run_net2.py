"""run_net2.py [seconds] [level] [--host-extra "<steps>"] [--client-extra "<steps>"] [--grep regex]

Two test copies on this PC in one network game of the engine's own: the host (test-m33) loads a story level
in single player and turns it into a server; the client (test-m33b, its socket on another UDP port) connects
to 127.0.0.1, gets the host's quick save and loads the level from it. Prints both logs without the noise.
"""
import re, sys
from lab import Copy, wait, require_memory

a = sys.argv[1:]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


host_extra = opt("--host-extra", "")
client_extra = opt("--client-extra", "")
grep = opt("--grep")
host_at = float(opt("--host-at", "8"))
join_at = float(opt("--join-at", "30"))
seconds = int(a[0]) if a else 90
level = a[1] if len(a) > 1 else r"2033\l01_hunter"

host_script = (r"0.3: >stop_video | +0.5: disconnect | +0.5: map %s | ?mapstate_running | +1: !unpause | +%g: !host | +1: !net"
               % (level, host_at))
if host_extra:
    host_script += " | " + host_extra
# The menu is a level too: it is started first (what a key press on the title screen does), then the join.
client_script = ("0.3: >stop_video | ?mapstate_loaded_and_waiting | +1: !finish | +1: !unpause | %g: !net"
                 " | +0.2: connect 127.0.0.1 | ?mapstate_loaded_and_waiting | +0.5: !mp 1"
                 " | +1: !finish | +1: !unpause | +3: !net" % join_at)
if client_extra:
    client_script += " | " + client_extra

require_memory(2)
host, client = Copy("test-m33"), Copy("test-m33b")
ph = host.start(seconds, host_script)
pc = client.start(seconds, client_script, bind_port=1001, autostart=False)
print("exit codes", wait([ph, pc], seconds + 240))

SKIP = ("[game] ^", "Unknown command", "RT create", "creating CB", "DX11: layout", "Invalid model", "import ",
        "AS ", "FX: Actor", "loader] progress", "BACKEND::ALLOC", "meshes_", "textures::", "PhysX", "colorgrade",
        " models ", "font:", "hooked ", "XAudio", "' [Ch:", "special folder", "decals:", "skybox", "envmap",
        "render]", "sound]", "shader", "particles", "Can't find model", "Missing model", "sound occlusion",
        "patcher-", "clouds changed")
for c in (host, client):
    lines = [l for l in c.log_lines() if not any(s in l for s in SKIP)]
    start = next((i for i, l in enumerate(lines) if "start-up is over" in l), 0)
    lines = lines[start:]
    if grep:
        rx = re.compile(grep, re.I)
        lines = [l for l in lines if rx.search(l)]
    # collapse runs of the same engine line
    out, last, rep = [], None, 0
    for l in lines:
        body = l[22:]
        if body == last:
            rep += 1
            continue
        if rep:
            out.append("        ... %d more of the same" % rep)
        out.append(l)
        last, rep = body, 0
    if rep:
        out.append("        ... %d more of the same" % rep)
    print("==== %s: %d lines" % (c.name, len(out)))
    for l in out[:170]:
        print(l[1:13] + l[19:240])
    if len(out) > 170:
        print("... %d more lines" % (len(out) - 170))
