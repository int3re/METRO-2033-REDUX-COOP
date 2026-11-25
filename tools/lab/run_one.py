"""run_one.py <seconds> "<script>" [--copy name] [--port n] [--grep regex] [--tail n] [-- game args]

One test copy of the game with the mod, off the screen. The script is "seconds: command | seconds: command"
(seconds from the end of the engine's start-up; a command is a console command or one of the lab's own: !net,
!byte <rva> <value>, !dword <rva> <value>, !peek <rva> [count])."""
import sys
from lab import Copy, show

a = sys.argv[1:]
game_args = []
if "--" in a:
    game_args = a[a.index("--") + 1:]
    a = a[:a.index("--")]


def opt(name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


copy = Copy(opt("--copy", "test-m33"))
port = opt("--port")
grep = opt("--grep")
tail = opt("--tail")
seconds = int(a[0])
script = a[1] if len(a) > 1 else ""
code = copy.run(seconds, script, int(port) if port is not None else None, game_args)
print("exit code", code)
show(copy, grep, int(tail) if tail else None)
