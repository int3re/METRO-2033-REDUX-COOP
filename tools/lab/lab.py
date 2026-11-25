"""The lab: test copies of Metro 2033 Redux and runs of them with the mod inside.

A copy is a folder next to the game's folder (test-m33, test-m33b ...): the game's big files are
hard links, the mod's files come from build/dist/Release, the user folder (config, saves) is the copy's
own `_user`, and metronet.ini has a [lab] section: the window is off the screen and never active, the
game gets no keyboard / mouse, and the process stops by itself.

    from lab import Copy
    c = Copy("test-m33")
    c.run(seconds=40, script="2: map 2033\\\\l00_intro", bind_port=None)
"""
import os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ROOT = os.path.dirname(REPO)
GAME = os.path.join(ROOT, "Metro 2033 Redux")
# (METRONET_DIST: a long sweep runs on a kept copy of the build while the build folder changes)
DIST = os.environ.get("METRONET_DIST") or os.path.join(REPO, "build", "dist", "Release")
MOD_FILES = ("dinput8.dll", "metronet.asi")
OPTIONAL_FILES = ("metronet_steam_api64.dll",)  # copied when the build has them
# METRONET_STEAM=1: every run goes through the mod's Steam path with the lab's stand-in for Steam - each copy
# is a stand-in user ([lab] steam = 1, 2, 3 by the copy's folder) and a joiner's "address = 127.0.0.1" becomes
# "address = steam" (it finds the host's lobby among its "friends"). Any test of the lab can be run this way.
# METRONET_AUTO=1: every run goes the way a player gets it - nobody sets a role ([net] mode = auto, through the
# stand-in for Steam). The host just plays; the joiner's game stands in the main menu, the game's own window asks
# "... is playing. Join this game?" and the joiner's script answers yes with the pad's A. Any test can run so.
AUTO = os.environ.get("METRONET_AUTO", "") not in ("", "0")
STEAM = AUTO or os.environ.get("METRONET_STEAM", "") not in ("", "0")
STEAM_USER = {"test-m33": 1, "test-m33b": 2, "test-m33c": 3}

# The story's levels in order ("map 2033\<name>"). l05_pogruzchik, l10_bump and l23_ejh are folders of the game, not levels.
LEVELS = """l00_intro l01_hunter l02_exhibition l03_chase l04_riga l05_lost_tunnel l08_market
l09_dead_city_1 l11_dry l12_ghosts l14_cursed l15_armory l16_frontline l17_trolley_combat l18_depot
l19_defence l21_nazi_outpost l22_black l23_polis l24_alley l26_depository l27_archives l28_driving
l30_darkstar l32_cave l33_d6 l36_ostankino""".split()

# The lab's own picture and sound settings (over the player's user.cfg).
CFG = {
    "r_fullscreen": "off",
    "r_res_hor": "640",
    "r_res_vert": "360",
    "r_quality_level": "0",
    "r_af_level": "0",
    "r_vsync": "off",
    "r_dx11_tess": "0",
    "r_msaa_level": "0",
    "r_supersample": "1.",
    "s_master_volume": "0.00",
    "s_music_volume": "0.00",
    "vibration": "0",
}


# What a run needs free per copy; it is not started when the PC has less to give. Measured 2026-11-23 (lab
# settings, l01_hunter): a copy peaks at 0.6 GB of RAM and 1.9 GB of commit. (2026-11-22: with Blender holding
# 8.7 GB, 1.5 GB of RAM and 3 GB of commit free, both copies died at start-up inside the game's allocator - and
# a run in that state could take the user's other programs down with it.)
NEED_RAM_GB, NEED_COMMIT_GB = 1.0, 2.8
_memory_checked = False


def free_memory_gb():
    """(free physical memory, free commit) in GB."""
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
                    ("total_phys", ctypes.c_ulonglong), ("avail_phys", ctypes.c_ulonglong),
                    ("total_commit", ctypes.c_ulonglong), ("avail_commit", ctypes.c_ulonglong),
                    ("total_virtual", ctypes.c_ulonglong), ("avail_virtual", ctypes.c_ulonglong),
                    ("avail_extended", ctypes.c_ulonglong)]

    st = Status()
    st.length = ctypes.sizeof(Status)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
    return st.avail_phys / 2.0 ** 30, st.avail_commit / 2.0 ** 30


USER_GAMES = ("dota2.exe", "cs2.exe")  # the user's own games: no lab copies while one runs (2026-11-26: Dota 2)


def user_game_running():
    """The name of a game the user plays now (one of USER_GAMES, or the real Metro outside the lab), or ''."""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=30).stdout.lower()
    except Exception:
        return ""
    for name in USER_GAMES:
        if '"%s"' % name in out:
            return name
    return ""


def require_memory(copies=1):
    """Stops the run (SystemExit) when the PC has no room for `copies` copies of the game."""
    global _memory_checked
    wait_min = float(os.environ.get("METRONET_WAIT_MEMORY", "0") or 0)
    playing = user_game_running()
    if playing and wait_min > 0:
        print("lab: waiting - the user plays (%s)" % playing, flush=True)
        end = time.time() + wait_min * 60
        while playing and time.time() < end:
            time.sleep(60)
            playing = user_game_running()
    if playing:
        raise SystemExit("lab: NOT STARTED - the user plays (%s)" % playing)
    ram, commit = free_memory_gb()
    # METRONET_WAIT_MEMORY = minutes: a long run in the background waits for room (with a margin - the user's own
    # programs come first) instead of stopping, e.g. while the user plays something (2026-11-24: a sweep "finished"
    # in a minute when a game was started - every level left was refused at once)
    wait_min = float(os.environ.get("METRONET_WAIT_MEMORY", "0") or 0)
    if wait_min > 0 and (ram < NEED_RAM_GB * copies + 1.5 or commit < NEED_COMMIT_GB * copies + 1.5):
        end, quiet = time.time() + wait_min * 60, 0
        print("lab: waiting for free memory (RAM %.1f GB, commit %.1f GB free now)" % (ram, commit), flush=True)
        while time.time() < end and quiet < 3:  # three looks half a minute apart: not a moment's dip
            time.sleep(30)
            ram, commit = free_memory_gb()
            quiet = quiet + 1 if ram >= NEED_RAM_GB * copies + 1.5 and commit >= NEED_COMMIT_GB * copies + 1.5 else 0
    if ram < NEED_RAM_GB * copies or commit < NEED_COMMIT_GB * copies:
        raise SystemExit("lab: NOT STARTED - too little free memory for %d game cop%s: RAM %.1f GB free (need %.1f), "
                         "commit %.1f GB free (need %.1f)" % (copies, "y" if copies == 1 else "ies", ram,
                                                              NEED_RAM_GB * copies, commit, NEED_COMMIT_GB * copies))
    _memory_checked = True


def _user_cfg():
    """The player's user.cfg as the base (keys, language), or the one in the game folder."""
    for p in (os.path.join(os.environ.get("LOCALAPPDATA", ""), "4A Games", "Metro 2033", "user.cfg"),
              os.path.join(GAME, "user.cfg")):
        if os.path.exists(p):
            return open(p, encoding="latin1").read().splitlines()
    return []


class Copy:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(ROOT, name)
        self.user = os.path.join(self.dir, "_user")
        self.log = os.path.join(self.dir, "metronet.log")

    def ensure(self):
        os.makedirs(self.dir, exist_ok=True)
        for base, dirs, files in os.walk(GAME):
            rel = os.path.relpath(base, GAME)
            dst_dir = os.path.join(self.dir, rel) if rel != "." else self.dir
            os.makedirs(dst_dir, exist_ok=True)
            for f in files:
                src, dst = os.path.join(base, f), os.path.join(dst_dir, f)
                if f.lower() in ("user.cfg", "metronet.ini", "metronet.log") or f.lower() in MOD_FILES:
                    continue
                if f.lower().startswith("unins") or f.lower().endswith((".ico", ".zip")):
                    continue
                if not os.path.exists(dst):
                    os.link(src, dst)
        for f in MOD_FILES:
            # (a copy killed a moment ago can still hold its files: 2026-11-24, after the PC had slept in the
            #  middle of a run, every next test fell here with "file in use")
            for attempt in range(30):
                try:
                    shutil.copy2(os.path.join(DIST, f), os.path.join(self.dir, f))
                    break
                except PermissionError:
                    if attempt == 29:
                        raise
                    time.sleep(1)
        for f in OPTIONAL_FILES:
            if os.path.exists(os.path.join(DIST, f)):
                try:
                    shutil.copy2(os.path.join(DIST, f), os.path.join(self.dir, f))
                except PermissionError:
                    pass  # held by a copy that is still closing; the stand-in does not need it
        cfg_dir = os.path.join(self.user, "4A Games", "Metro 2033")
        os.makedirs(cfg_dir, exist_ok=True)
        lines, seen = [], set()
        for line in _user_cfg():
            key = line.split(" ", 1)[0]
            if key in CFG:
                line = "%s %s" % (key, CFG[key])
                seen.add(key)
            lines.append(line)
        lines += ["%s %s" % (k, v) for k, v in CFG.items() if k not in seen]
        for p in (os.path.join(cfg_dir, "user.cfg"), os.path.join(self.dir, "user.cfg")):
            open(p, "w", encoding="latin1", newline="\r\n").write("\n".join(lines) + "\n")

    def write_ini(self, seconds, script="", bind_port=None, extra="", autostart=True):
        ini = ["[log]", "level = debug", "engine = 1", "", "[lab]", "enabled = 1",
               "userdir = %s" % self.user, "quit_after = %d" % seconds]
        if bind_port is not None:
            ini.append("bind_port = %d" % bind_port)
        if not autostart:
            ini.append("autostart = 0")
        if script:
            ini.append("script = %s" % script)
        # METRONET_LAB="cores = 8;fps = 30": more [lab] lines for every copy of a run
        if os.environ.get("METRONET_LAB"):
            ini += [l.strip() for l in os.environ["METRONET_LAB"].split(";") if l.strip()]
        if STEAM:
            ini.append("steam = %d" % STEAM_USER.get(self.name, 9))
            extra = extra.replace("address = 127.0.0.1", "address = steam")
        if AUTO:
            import re
            extra = re.sub(r"mode = (host|join)", "mode = auto", extra)
            extra = re.sub(r"address = \S+\n?", "", extra)
        if extra:
            ini += ["", extra]
            # METRONET_NET="ride = 1;friendly_fire = 1": more [net] lines for every copy of a run (the callers'
            # extra ends inside [net])
            if os.environ.get("METRONET_NET") and "[net]" in extra:
                ini += [l.strip() for l in os.environ["METRONET_NET"].split(";") if l.strip()]
        open(os.path.join(self.dir, "metronet.ini"), "w", encoding="utf-8", newline="\r\n").write("\n".join(ini) + "\n")

    def start(self, seconds, script="", bind_port=None, args=(), extra="", autostart=True, after=None):
        """after=(host copy, its process): this copy is a joiner. It is started when that host hosts its level
        (see wait_hosting) and gets the time that is left on the host's clock, so both end together; the
        "20: !finish" of its script (the margin the old start, both at once, needed) becomes "8: !finish"."""
        if not _memory_checked:
            require_memory(1)
        if after is not None:
            host, proc = after
            wait_hosting(host, proc)
            late = time.time() - host.started
            seconds = max(36, int(round(seconds - late)))
            script = script.replace("20: !finish", "8: !finish")
            # A step at a fixed second ("65: !world 1") was written for two copies started together and must
            # come at the same moment as the host's: on this copy's clock that is `late` seconds earlier.
            import re

            def shift(m):
                t = float(m.group(2))
                return m.group(1) + ("%.1f: " % max(9.0, t - late) if t > 20 else m.group(2) + ": ")
            script = re.sub(r"(^|\| )(\d+(?:\.\d+)?): ", shift, script)
        if AUTO and "mode = join" in extra:
            # the joiner's own part: past "press any key", then yes to the game's question
            # (whatever second the test gives its "!finish": three.py starts its two joiners a little apart)
            import re
            script = re.sub(r"(\d+: !finish)", r"\1 | ?Activate menu [message_menu] | +1.5: !pad 1000 0 0 200", script, count=1)
        self.ensure()
        self.write_ini(seconds, script, bind_port, extra, autostart)
        if os.path.exists(self.log):
            os.remove(self.log)
        shutil.rmtree(os.path.join(self.dir, "shots"), ignore_errors=True)  # the pictures of the run before
        self.started = time.time()
        return subprocess.Popen([os.path.join(self.dir, "metro.exe"), *args], cwd=self.dir)

    def run(self, seconds, script="", bind_port=None, args=(), extra=""):
        p = self.start(seconds, script, bind_port, args, extra)
        return wait([p], seconds + 240)[0]

    def log_lines(self):
        if not os.path.exists(self.log):
            return []
        return open(self.log, encoding="utf-8", errors="replace").read().splitlines()


def _process_memory_gb(handle):
    """(peak working set, peak commit) of a process in GB, or None."""
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD),
                    ("peak_ws", ctypes.c_size_t), ("ws", ctypes.c_size_t),
                    ("peak_paged", ctypes.c_size_t), ("paged", ctypes.c_size_t),
                    ("peak_nonpaged", ctypes.c_size_t), ("nonpaged", ctypes.c_size_t),
                    ("pagefile", ctypes.c_size_t), ("peak_pagefile", ctypes.c_size_t)]

    c = Counters()
    c.cb = ctypes.sizeof(Counters)
    get = ctypes.windll.psapi.GetProcessMemoryInfo
    get.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    if not get(int(handle), ctypes.byref(c), c.cb):
        return None
    return c.peak_ws / 2.0 ** 30, c.peak_pagefile / 2.0 ** 30


def wait_hosting(host, proc, limit=180):
    """Waits until the host copy says it hosts its level - a joiner started before that knocks at a closed door
    and gives up ("no answer in 60 s"; 2026-11-24: with the copies slowed to 30 frames a second the big levels
    load in 20-35 s, and a joiner that knocked at its 20th second was early on 20 levels of 27).
    Returns the seconds since the call."""
    t0 = time.time()
    while time.time() - t0 < limit and proc.poll() is None:
        # (mode = auto: the level is hosted only when somebody comes; the host is ready when its lobby is open)
        mark = "steam: this game is open to your Steam friends" if AUTO else "session: hosting the level"
        if any(mark in l for l in host.log_lines()):
            break
        time.sleep(0.5)
    return time.time() - t0


def _stay_low(handle):
    """A test copy runs at below-normal priority. The mod's lab code sets it at the start, but the game puts
    itself back to normal while it loads, so the runner sets it again every second."""
    import ctypes
    from ctypes import wintypes
    k = ctypes.windll.kernel32
    k.GetPriorityClass.argtypes = [wintypes.HANDLE]
    k.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    BELOW_NORMAL = 0x4000
    try:
        if k.GetPriorityClass(int(handle)) != BELOW_NORMAL:
            k.SetPriorityClass(int(handle), BELOW_NORMAL)
    except Exception:
        pass


def wait(procs, limit):
    """Waits for the processes; one that outlives `limit` seconds is killed. Returns exit codes.
    Prints what each took at its peak (the numbers behind NEED_RAM_GB / NEED_COMMIT_GB)."""
    end = time.time() + limit
    peak = [None] * len(procs)
    while any(p.poll() is None for p in procs) and time.time() < end:
        for i, p in enumerate(procs):
            if p.poll() is None:
                peak[i] = _process_memory_gb(p._handle) or peak[i]
                _stay_low(p._handle)
        time.sleep(1)
    codes = []
    for p in procs:
        if p.poll() is None:
            p.kill()
            try:
                p.wait(30)  # ... and gone, so that the next run finds its files free
            except Exception:
                pass
            codes.append("killed")
        else:
            codes.append(p.returncode)
    print("lab: peak memory per copy (RAM / commit, GB): " +
          ", ".join("%.1f / %.1f" % m if m else "?" for m in peak))
    return codes


def shots(copy, out_dir, gain=1.0, gamma=1.0):
    """The copy's pictures (shots/*.bmp, written by the script command "!shot <name>") as .png files in out_dir,
    brightened `gain` times and by a gamma curve (below 1 lifts the dark). Returns the paths in the order the pictures were taken."""
    from PIL import Image
    src = os.path.join(copy.dir, "shots")
    if not os.path.isdir(src):
        return []
    os.makedirs(out_dir, exist_ok=True)
    names = [f for f in os.listdir(src) if f.lower().endswith(".bmp")]
    names.sort(key=lambda f: os.path.getmtime(os.path.join(src, f)))
    out = []
    for f in names:
        im = Image.open(os.path.join(src, f)).convert("RGB")
        if gain != 1.0 or gamma != 1.0:
            im = im.point(lambda v: min(255, int(255 * (v / 255.0) ** gamma * gain)))
        p = os.path.join(out_dir, f[:-4] + ".png")
        im.save(p)
        out.append(p)
    return out


def sheet(files, path, columns=4, width=360):
    """One picture of many: the files as a grid with their names. Returns `path`."""
    from PIL import Image, ImageDraw
    ims = [Image.open(f) for f in files]
    h = max(im.height * width // im.width for im in ims)
    rows = (len(ims) + columns - 1) // columns
    out = Image.new("RGB", (columns * width, rows * (h + 14)), (30, 30, 30))
    draw = ImageDraw.Draw(out)
    for i, (im, f) in enumerate(zip(ims, files)):
        x, y = (i % columns) * width, (i // columns) * (h + 14)
        out.paste(im.resize((width, im.height * width // im.width)), (x, y + 14))
        draw.text((x + 3, y + 1), os.path.splitext(os.path.basename(f))[0], fill=(255, 255, 255))
    out.save(path)
    return path


def show(copy, grep=None, tail=None, skip=("[game] ~",)):
    lines = copy.log_lines()
    if grep:
        import re
        rx = re.compile(grep, re.I)
        lines = [l for l in lines if rx.search(l)]
    if tail:
        lines = lines[-tail:]
    print("---- %s: %d lines" % (copy.name, len(lines)))
    for l in lines:
        print(l[:260])
