"""Scratch runner of the first experiments: one copy, a script given here (backslashes survive in a file)."""
import sys
from lab import Copy, show

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 45
script = sys.argv[2] if len(sys.argv) > 2 else r"1: stop_video | 3: disconnect | 4: map 2033\l00_intro | 30: !net"
grep = sys.argv[3] if len(sys.argv) > 3 else None
c = Copy("test-m33")
print("exit code", c.run(seconds, script))
skip = ("[game] ^", "Unknown command", "RT create", "creating CB", "DX11: layout", "Invalid model", "import ", "AS ", "FX: Actor")
lines = [l for l in c.log_lines() if not any(s in l for s in skip)]
if grep:
    import re
    rx = re.compile(grep, re.I)
    lines = [l for l in lines if rx.search(l)]
start = next((i for i, l in enumerate(lines) if "start-up is over" in l), 0)
for l in lines[start:]:
    print(l[15:245])
