"""wait_memory.py [copies] [minutes]

Waits until the PC has room for `copies` test copies with a margin (the user's own programs come first) and
prints one line; gives up after `minutes` (default 180). For starting a long test run in the background only
when it will not push the PC to its limit.
"""
import sys, time
from lab import free_memory_gb, NEED_RAM_GB, NEED_COMMIT_GB

copies = int(sys.argv[1]) if len(sys.argv) > 1 else 2
minutes = int(sys.argv[2]) if len(sys.argv) > 2 else 180
need_ram, need_commit = NEED_RAM_GB * copies + 1.5, NEED_COMMIT_GB * copies + 1.5
end = time.time() + minutes * 60
quiet = 0
while time.time() < end:
    ram, commit = free_memory_gb()
    quiet = quiet + 1 if ram >= need_ram and commit >= need_commit else 0
    if quiet >= 3:  # three looks a minute apart: not a moment's dip in somebody's use
        print("room: RAM %.1f GB, commit %.1f GB free (asked %.1f / %.1f)" % (ram, commit, need_ram, need_commit))
        sys.exit(0)
    time.sleep(60)
ram, commit = free_memory_gb()
print("no room in %d minutes: RAM %.1f GB, commit %.1f GB free (asked %.1f / %.1f)" % (minutes, ram, commit, need_ram, need_commit))
sys.exit(1)
