"""metronet-launcher.exe against a stand-in game folder, a stand-in user folder and a stand-in for GitHub.

    python tools/lab/launcher_lab.py [only <scenario> ...]

Nothing of the real game is used and nothing leaves the PC: everything lives under build/launcher-lab/run.
The "game" there is a folder with a small file named metro.exe that has the PE header of the build the mod
knows (src/mod/game.h) - it is no program and is never started; a second folder has a metro.exe of "another
build". The game's own settings are a user.cfg written here, in a folder of the lab (--userdir): the real
%LOCALAPPDATA%\\4A Games\\Metro 2033 is never read. The launcher runs as a copy (it replaces its own file in
some of the runs), with its own settings and cache, under a script (--drive): its window is never shown, the
pointer and the keyboard are not touched, no prompt of Windows is raised, the game is never started.
"GitHub" is tools/lab/launcher_feed.py on 127.0.0.1.

Scenarios: where, install, keep-ini, settings, bare-ini, uninstall, window-install, wrong-build, pick,
by-hand, steam, window, graphics, controls, cfg-guard, no-release, no-answer, update, bad-sum, old-settings, self-update,
game-open, pictures. A line "ok" or "FAIL" for every check; the exit code is the number of failed checks.
"""
import ctypes
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import launcher_feed  # noqa: E402
import launcher_newer  # noqa: E402

DIST = os.path.join(REPO, "build", "dist", "Release")
BUILT = os.path.join(DIST, "metronet-launcher.exe")
WORK = os.path.join(REPO, "build", "launcher-lab", "run")
GAME, OTHER, APP, FEED, USER = (os.path.join(WORK, n) for n in ("game", "game-other", "app", "feed", "user"))
MINE = os.path.join(APP, "metronet-launcher.exe")
CFG = os.path.join(USER, "user.cfg")
CFG_BACKUP = os.path.join(USER, "user.cfg.metronet-backup")
MOD_FILES = ["dinput8.dll", "metronet.asi", "metronet_steam_api64.dll", "metronet-README.md", "metronet.ini"]
LIST = "metronet-install.txt"

failed = 0
checks = 0


def check(what, ok):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
    print("   %s  %s" % ("ok  " if ok else "FAIL", what), flush=True)
    return ok


def source(path):
    return io.open(os.path.join(REPO, path), encoding="utf-8").read()


MOD_VERSION = re.search(r'METRONET_VERSION "([^"]+)"', source("src/common/version.h")).group(1)
STAMP = int(re.search(r"kTimeDateStamp = (0x[0-9A-Fa-f]+)", source("src/mod/game.h")).group(1), 16)
IMAGE = int(re.search(r"kSizeOfImage = (0x[0-9A-Fa-f]+)", source("src/mod/game.h")).group(1), 16)
INSIDE = {}  # the mod's files as this launcher carries them: name -> bytes


def read_inside():
    """What is inside the built launcher (the lab checks an install against that, not against a build folder
    that somebody may be building into)."""
    index = launcher_newer.res(BUILT, 300)[0].decode("utf-8")
    for line in index.splitlines():
        words = line.split()
        if len(words) >= 3 and words[0] == "file":
            INSIDE[words[1]] = launcher_newer.res(BUILT, int(words[2]))[0]
    stale = [n for n in INSIDE if os.path.exists(os.path.join(DIST, n)) and open(os.path.join(DIST, n), "rb").read() != INSIDE[n]]
    if stale:
        print("   --    build/dist has another %s than this launcher carries: the mod was built after the launcher" % ", ".join(stale))


def fake_exe(path, stamp, image):
    """A file with the header of a 64-bit program: the two numbers the launcher tells the game's build by."""
    d = bytearray(0x400)
    d[0:2] = b"MZ"
    struct.pack_into("<I", d, 0x3C, 0x80)
    d[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHI", d, 0x84, 0x8664, 0, stamp)  # machine, sections, TimeDateStamp
    struct.pack_into("<H", d, 0x80 + 20, 0xF0)           # the size of the optional header
    struct.pack_into("<H", d, 0x80 + 24, 0x20B)          # PE32+
    struct.pack_into("<I", d, 0x80 + 24 + 56, image)     # SizeOfImage
    open(path, "wb").write(d)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def sha(path):
    return hashlib.sha256(read(path)).hexdigest()


def fresh_app():
    os.makedirs(APP, exist_ok=True)
    for name in os.listdir(APP):
        os.remove(os.path.join(APP, name))
    shutil.copyfile(BUILT, MINE)


def launcher(args, timeout=60):
    """One run of the launcher's copy without a window; its exit code (None: it did not end and was stopped)."""
    p = subprocess.Popen([MINE] + args, cwd=APP)
    try:
        return p.wait(timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return None


def log_of(name):
    path = os.path.join(WORK, name + ".log")
    return io.open(path, encoding="utf-8", errors="replace").read() if os.path.exists(path) else ""


def drive(name, script, game=GAME, repo=None, auto_self=False, auto_check=True, pick=None, steam=1, running=0, userdir=USER,
          next_script=None, timeout=90, ends=1, may_miss=False, old_settings=False):
    """A hidden run of the launcher under a script; its log. `ends`: how many launchers are to end (a
    self-update starts a second one in the first one's place). may_miss: a step that finds nothing to press
    is what the scenario is about."""
    ini, log = os.path.join(WORK, name + ".ini"), os.path.join(WORK, name + ".log")
    for f in (ini, log):
        if os.path.exists(f):
            os.remove(f)
    io.open(ini, "w", encoding="utf-16").write(
        "[launcher]\n%sgame_dir=%s\nauto_check=%d\nauto_self=%d\nauto_install=0\nclose_on_play=0\nproxy=1\n" %
        ("" if old_settings else "settings=2\n", game, 1 if auto_check else 0, 1 if auto_self else 0))
    args = ["--drive", script, "--settings", ini, "--cache", os.path.join(WORK, "cache"), "--out", log, "--userdir", userdir]
    if steam is not None:  # (None: the launcher looks at this PC's processes itself)
        args += ["--steam", str(steam)]
    if running is not None:
        args += ["--game-running", str(running)]
    if repo:
        args += ["--repo", repo]
    if pick:
        args += ["--pick", pick]
    if next_script:
        args += ["--drive-next", next_script]
    rc = launcher(args, timeout)
    until = time.time() + timeout
    while log_of(name).count("] exit") < ends and time.time() < until:  # (the launcher started in the first one's place)
        time.sleep(0.2)
    text = log_of(name)
    if rc is None or text.count("] exit") < ends:
        check("%s: the launcher ended by itself" % name, False)
    if "TIMEOUT" in text or ("NOT ON THIS SCREEN" in text and not may_miss):
        check("%s: every step of the script found its place (%s)" % (name, "; ".join(
            line.split("] ", 1)[-1] for line in text.splitlines() if "NOT ON THIS SCREEN" in line or "TIMEOUT" in line)), False)
    return text


class States(list):
    """The states a script logged; one that is not there reads as an empty line (its checks then fail)."""

    def __getitem__(self, i):
        try:
            return list.__getitem__(self, i)
        except IndexError:
            return ""


def states(text):
    """The "state:" lines of a log, joined per call of `state`."""
    out, cur = States(), []
    for line in text.splitlines():
        m = re.search(r"\] state: (.*)", line)
        if not m:
            continue
        if m.group(1).startswith("screen "):
            cur = []
            out.append(cur)
        cur.append(m.group(1))
    return States(" | ".join(s) for s in out)


def lines_of(text, tag):
    """What the launcher logged under a tag ("file: ...", "cfg: ..."), in order."""
    return [m.group(1) for m in re.finditer(r"(?m)^\[\d+\] %s: (.*?)\r?$" % tag, text)]


# --- metronet.ini as the mod reads it (src/mod/config.cpp) ---------------------------------------------

def net_value(data, key):
    inside = False
    for raw in data.decode("utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("["):
            if inside:
                break
            inside = line.lower().startswith("[net]")
            continue
        if not inside or line.startswith(";") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k.strip().lower() != key:
            continue
        v = v.strip()
        if v.startswith(";"):
            return ""
        return v.split(" ;")[0].rstrip()
    return None


def comment_of(line):
    """(what stands before the comment, the comment's column in letters, the comment) of a "key = value ; comment" line."""
    s = line.decode("utf-8").rstrip("\r")
    eq = s.index("=")
    rest = s[eq + 1:]
    if rest.lstrip(" \t").startswith(";"):
        c = len(rest) - len(rest.lstrip(" \t"))
    else:
        c = rest.find(" ;") + 1 if " ;" in rest else -1
    if c < 0:
        return s, -1, ""
    return s[:eq + 1 + c].rstrip(), eq + 1 + c, rest[c:]


def mod_installed(game):
    return [n for n in MOD_FILES + [LIST] if os.path.exists(os.path.join(game, n))]


def installed_version(game):
    path = os.path.join(game, LIST)
    if not os.path.exists(path):
        return ""
    m = re.match(r"# metronet ([0-9.]+)", io.open(path, encoding="utf-8").readline())
    return m.group(1).rstrip(".") if m else "?"


def headless(verb, game, name):
    log = os.path.join(WORK, name + ".log")
    if os.path.exists(log):
        os.remove(log)
    return launcher([verb, game, "/quiet", "--out", log])


# --- the game's user.cfg, as the game writes one (the names and the keys are the game's; the values are the lab's) ---

BINDS = """changemenumap kESCAPE|wpn_2 k1|wpn_3 k2|special k3|wpn_4 k4|wpn_5 k5|popup_weapons kTAB|medkit kQ|forward kW|use kE|wpn_reload kR|
filter kT|time kY|wpn_next kLBRACKET|wpn_prev kRBRACKET|menu_enter kRETURN|crouch kLCONTROL|lstrafe kA|back kS|rstrafe kD|wpn_light kF|
gasmask kG|console kGRAVE|sprint kLSHIFT|crouch_toggle kZ|accel kX|wpn_dynamite kC|wpn_fast_knife kV|nightvision kN|map kM|jump kSPACE|
retranslate kF5|cam_zoom_out kSUBTRACT|cam_zoom_in kADD|pause kPAUSE|up kUP|left kLEFT|right kRIGHT|down kDOWN|wpn_fire mouse0|
wpn_aim mouse1|wpn_knife mouse2""".replace("\n", "").split("|")
USER_CFG = ("\r\n".join(
    ["_gamepad_preset_sens 0", "_show_subtitles 1", "aim_assist 1.", "auto_aim_border 0.25", "unbindall"] +
    ["bind " + b for b in BINDS] +
    ["fps off", "g_game_difficulty 0", "g_show_crosshair on", "invert_y_axis off", "joy_sens_x 0.40000001", "lang_text ru",
     "mouse_curve_coef 70.", "name must", "r_af_level 1", "r_api 2", "r_base_fov 50.625", "r_blur_level 1", "r_dx11_tess 0",
     "r_enum_ssaa 10", "r_fullscreen off", "r_gamma 1.", "r_gi_volume 1.", "r_quality_level 1", "r_res_hor 1284", "r_res_vert 722",
     "r_ssaa_auto 0.", "r_supersample 1.", "r_tess_ss 0", "r_vsync off", "s_master_volume 0.50", "sens 0.4", "sens_border 0.5",
     "show_game_ui 1", "vibration 2", "xbox_net_data (" + ",".join("%x=0" % i for i in range(3, 300)) + ")", "xbox_state_data ()()"]) +
    "\r\n").encode("ascii")


def fresh_user(with_file=True):
    if os.path.exists(USER):
        shutil.rmtree(USER)
    os.makedirs(USER)
    if with_file:
        open(CFG, "wb").write(USER_CFG)


def cfg_line(data, begins):
    for line in data.decode("ascii").split("\r\n"):
        if line.startswith(begins + " "):
            return line
    return None


def changed_names(before, after):
    """The first words of the lines that differ ("bind <action>" for a key's line); None: another number of lines."""
    a, b = before.split(b"\r\n"), after.split(b"\r\n")
    if len(a) != len(b):
        return None
    out = set()
    for x, y in zip(a, b):
        if x != y:
            words = x.decode("ascii").split(" ")
            out.add(" ".join(words[:2]) if words[0] == "bind" else words[0])
    return out


# --- the scenarios ------------------------------------------------------------------------------------

def prepare():
    if os.path.exists(WORK):
        shutil.rmtree(WORK)
    for d in (GAME, OTHER, APP, FEED, os.path.join(WORK, "cache"), os.path.join(WORK, "shots")):
        os.makedirs(d)
    fake_exe(os.path.join(GAME, "metro.exe"), STAMP, IMAGE)
    fake_exe(os.path.join(OTHER, "metro.exe"), STAMP + 86400, IMAGE + 0x1000)
    for g in (GAME, OTHER):
        open(os.path.join(g, "steam_api64.dll"), "wb").write(b"the game's own steam_api64.dll (a stand-in)\n" * 40)
    open(os.path.join(GAME, "dinput8.dll"), "wb").write(b"somebody else's dinput8.dll\n" * 30)
    fresh_app()
    fresh_user()


def s_where():
    print("where: the game is looked for (the registry is only read)")
    log = os.path.join(WORK, "where.log")
    rc = launcher(["--where", "--out", log])
    text = log_of("where")
    check("--where ends (exit code %s)" % rc, rc in (0, 1))
    check("the folder beside the launcher with the right metro.exe is seen as the game",
          ("beside the launcher: %s (the game)" % GAME) in text)
    check("the folder with another build's metro.exe is not offered", ("game: %s" % OTHER) not in text)
    gog = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\GOG.com\Games\1436434037", 0, winreg.KEY_READ | winreg.KEY_WOW64_32KEY) as k:
            gog = winreg.QueryValueEx(k, "path")[0]
    except OSError:
        pass
    if gog:
        check("GOG's record of the game in the registry is read (it names a folder that %s)" %
              ("is there" if os.path.exists(os.path.join(gog, "metro.exe")) else "is not on this PC any more"),
              ("GOG record, path: %s" % gog.rstrip("\\")) in text)
    else:
        print("   --    this PC has no GOG record of the game in the registry: reading it is not checked")


def s_install():
    print("install: /install puts the mod's files next to metro.exe")
    api, exe, foreign = (read(os.path.join(GAME, n)) for n in ("steam_api64.dll", "metro.exe", "dinput8.dll"))
    rc = headless("/install", GAME, "install")
    check("exit code 0 (%s)" % rc, rc == 0)
    for n in MOD_FILES:
        if n not in INSIDE:
            print("   --    the launcher carries no %s" % n)
            continue
        check("%s is there, byte for byte the one inside the launcher" % n,
              os.path.exists(os.path.join(GAME, n)) and read(os.path.join(GAME, n)) == INSIDE[n])
    check("the list says %s (%s)" % (MOD_VERSION, installed_version(GAME)), installed_version(GAME) == MOD_VERSION)
    check("the game's own steam_api64.dll is as it was", read(os.path.join(GAME, "steam_api64.dll")) == api)
    check("metro.exe is as it was", read(os.path.join(GAME, "metro.exe")) == exe)
    kept = os.path.join(GAME, "metronet-backup", "dinput8.dll")
    check("somebody else's dinput8.dll is kept in metronet-backup", os.path.exists(kept) and read(kept) == foreign)


OWN_INI = ("; my own settings\r\n[net]\r\nmode = off   ; I play alone for now\r\nname = Ульман\r\n\r\n[log]\r\nlevel = debug\r\n").encode("utf-8")


def s_keep_ini():
    print("keep-ini: an ini that is there is never overwritten")
    path = os.path.join(GAME, "metronet.ini")
    open(path, "wb").write(OWN_INI)
    rc = headless("/install", GAME, "keep-ini")
    check("exit code 0 (%s)" % rc, rc == 0)
    check("metronet.ini is the player's own, byte for byte", read(path) == OWN_INI)
    check("the launcher says the settings were kept", "оставлены ваши настройки" in log_of("keep-ini"))
    check("the other files are the launcher's again", read(os.path.join(GAME, "metronet.asi")) == INSIDE["metronet.asi"])


def s_settings(feed):
    print("settings: the СЕТЬ station writes [net] of metronet.ini line by line and reads it back")
    path = os.path.join(GAME, "metronet.ini")
    template = INSIDE["metronet.ini"]
    open(path, "wb").write(template)
    text = drive("settings", "until:ready:20000;click:tab.net;state;click:coop;state;click:coop;click:names;click:ff;click:name;"
                 "type:Артём Тёмный;key:enter;click:ip.next;click:ip.next;click:address;type:192.168.1.5;key:enter;state;quit", repo=feed.url)
    st = states(text)
    check("the page shows the template's values at first (%s)" % st[0], len(st) == 3 and "mode auto names 1 ff 0 name \"\" address \"\"" in st[0])
    check("the switch turns the co-op off: mode = off", len(st) == 3 and "mode off" in st[1])
    data = read(path)
    got = {k: net_value(data, k) for k in ("mode", "names", "friendly_fire", "name", "address")}
    want = {"mode": "join", "names": "0", "friendly_fire": "1", "name": "Артём Тёмный", "address": "192.168.1.5"}
    check("the file has what was set (%s)" % got, got == want)
    check("the name is UTF-8 in the file", "name = Артём Тёмный".encode("utf-8") in data)
    a, b = template.split(b"\n"), data.split(b"\n")
    changed = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    keys = sorted(a[i].split(b"=")[0].strip().decode() for i in changed)
    check("only the five lines changed, the file has as many lines as before (%s)" % keys,
          len(a) == len(b) and keys == ["address", "friendly_fire", "mode", "name", "names"])
    was, now = [comment_of(a[i]) for i in changed], [comment_of(b[i]) for i in changed]
    check("the comments of the changed lines stayed, in their columns (or two spaces after a longer value)",
          all(w[2] == n[2] and (n[1] == w[1] or n[1] == len(n[0]) + 2) for w, n in zip(was, now)) and any(w[2] for w in was))
    text = drive("settings2", "until:ready:20000;click:tab.net;state;click:ip.prev;state;click:ip.prev;state;quit", repo=feed.url)
    st = states(text)
    check("a new start reads the same values back (%s)" % st[0].split(" | ")[1:2],
          len(st) == 3 and "mode join names 0 ff 1 name \"Артём Тёмный\" address \"192.168.1.5\"" in st[0])
    check("Я ХОСТ writes mode = host, НЕ НУЖНА brings mode = auto back",
          len(st) == 3 and "mode host" in st[1] and "mode auto" in st[2] and net_value(read(path), "mode") == "auto")
    check("the address is kept in the file for the next time", net_value(read(path), "address") == "192.168.1.5")


def s_bare_ini(feed):
    print("bare-ini: a file without the lines, a file without the section, a file of Notepad's Unicode")
    path = os.path.join(GAME, "metronet.ini")
    open(path, "wb").write(b"; mine\n[net]\nmode = auto\n\n[log]\nlevel = warn   ; quiet\n")
    drive("bare1", "until:ready:20000;click:tab.net;click:names;click:ff;state;quit", repo=feed.url)
    check("the missing lines are added to [net], with the file's own line ends, [log] stays",
          read(path) == b"; mine\n[net]\nmode = auto\nnames = 0\nfriendly_fire = 1\n\n[log]\nlevel = warn   ; quiet\n")
    open(path, "wb").write(b"; no net section here\r\n[log]\r\nlevel = warn\r\n")
    text = drive("bare2", "until:ready:20000;click:tab.net;state;click:coop;state;quit", repo=feed.url)
    st = states(text)
    check("no [net] at all reads as the mod's defaults", len(st) == 2 and "mode auto names 1 ff 0" in st[0])
    check("the section is added at the end with the line",
          read(path) == b"; no net section here\r\n[log]\r\nlevel = warn\r\n\r\n[net]\r\nmode = off\r\n")
    open(path, "wb").write(b"\xff\xfe" + "[net]\r\nmode = off\r\nname = Хан\r\n".encode("utf-16-le"))
    text = drive("bare3", "until:ready:20000;click:tab.net;state;click:coop;state;quit", repo=feed.url)
    st = states(text)
    check("a UTF-16 file is read (%s)" % st[0].split(" | ")[1:2], len(st) == 2 and "mode off" in st[0] and "name \"Хан\"" in st[0])
    check("... and stays UTF-16 when a line changes",
          read(path) == b"\xff\xfe" + "[net]\r\nmode = auto\r\nname = Хан\r\n".encode("utf-16-le"))
    open(path, "wb").write(INSIDE["metronet.ini"])


def s_uninstall(feed):
    print("uninstall: УДАЛИТЬ on the ЛАУНЧЕР station takes the mod's files out and nothing else")
    api, exe = read(os.path.join(GAME, "steam_api64.dll")), read(os.path.join(GAME, "metro.exe"))
    open(os.path.join(GAME, "metronet.log"), "wb").write(b"a log of a game\n")
    foreign = read(os.path.join(GAME, "metronet-backup", "dinput8.dll"))
    text = drive("uninstall", "until:ready:20000;click:tab.launcher;click:uninstall;wait:300;click:uninstall;until:install:30000;state;quit",
                 repo=feed.url)
    left = mod_installed(GAME) + [n for n in ("metronet.log", "metronet-backup") if os.path.exists(os.path.join(GAME, n))]
    check("the mod's files, its list, its log and the backup folder are gone (%s)" % (left or "nothing left"),
          left in ([], ["dinput8.dll"]) and "job: uninstall" in text)
    check("somebody else's dinput8.dll is back in its place", os.path.exists(os.path.join(GAME, "dinput8.dll")) and
          read(os.path.join(GAME, "dinput8.dll")) == foreign)
    check("the game's own steam_api64.dll and metro.exe are as they were",
          read(os.path.join(GAME, "steam_api64.dll")) == api and read(os.path.join(GAME, "metro.exe")) == exe)
    st = states(text)
    check("the launcher then offers to install (%s)" % st[-1].split(" | ")[0], "screen install" in st[-1] and "mod none" in st[-1])


def s_window_install(feed):
    print("window-install: the main action installs the mod that is inside the launcher")
    text = drive("window-install", "until:install:20000;click:primary;until:ready:30000;state;quit", repo=feed.url)
    st = states(text)
    check("job: install %s from this launcher" % MOD_VERSION, ("job: install %s from this launcher" % MOD_VERSION) in text)
    check("then: ready, mod %s (%s)" % (MOD_VERSION, st[-1].split(" | ")[0]), "screen ready" in st[-1] and ("mod %s " % MOD_VERSION) in st[-1])
    check("a fresh metronet.ini is the mod's template", read(os.path.join(GAME, "metronet.ini")) == INSIDE["metronet.ini"])
    check("the list says %s" % MOD_VERSION, installed_version(GAME) == MOD_VERSION)


def s_wrong_build(feed):
    print("wrong-build: another build of metro.exe is told plainly and nothing is installed")
    before = sorted(os.listdir(OTHER))
    rc = headless("/install", OTHER, "wrong-headless")
    check("/install refuses: exit code 2 (%s)" % rc, rc == 2)
    check("... and says why", "другая сборка игры" in log_of("wrong-headless"))
    text = drive("wrong-build", "until:wrongbuild:20000;state;click:tab.launcher;click:reinstall;wait:300;click:tab.game;state;quit",
                 game=OTHER, repo=feed.url, may_miss=True)
    st = states(text)
    check("the window stands on the page of another build (%s)" % st[0].split(" | ")[0],
          len(st) == 2 and "screen wrongbuild" in st[0] and "exe 0" in st[0])
    check("the ЛАУНЧЕР station offers no install there", "click reinstall: NOT ON THIS SCREEN" in text and "job: install" not in text and
          "screen wrongbuild" in st[-1])
    check("nothing was put into that folder", sorted(os.listdir(OTHER)) == before)


def s_pick(feed):
    print("pick: the folder is named by hand")
    text = drive("pick-none", "until:wrongbuild:20000;click:primary;wait:500;state;quit", game=OTHER, pick=os.path.join(WORK, "cache"),
                 repo=feed.url)
    check("a folder without metro.exe is not taken", "В этой папке нет metro.exe" in text and "screen wrongbuild" in states(text)[-1])
    text = drive("pick", "until:wrongbuild:20000;click:tab.launcher;click:folder;click:tab.game;until:ready:20000;state;quit", game=OTHER,
                 pick=GAME + "\\", repo=feed.url)
    st = states(text)
    check("ИЗМЕНИТЬ on the ЛАУНЧЕР station takes the right folder and its mod is seen (%s)" % st[-1].split(" | ")[0],
          "screen ready" in st[-1] and ("mod %s " % MOD_VERSION) in st[-1])
    saved = io.open(os.path.join(WORK, "pick.ini"), encoding="utf-16").read()
    check("... and it is remembered in the launcher's settings, without the slash at its end", ("game_dir=%s\n" % GAME) in saved.replace("\r", ""))


def s_by_hand(feed):
    print("by-hand: a mod that was put into the folder by hand, without the launcher's list")
    headless("/uninstall", GAME, "by-hand-clean")
    for n, data in INSIDE.items():
        open(os.path.join(GAME, n), "wb").write(data)
    api = read(os.path.join(GAME, "steam_api64.dll"))
    st = states(drive("by-hand", "until:ready:20000;state;quit", repo=feed.url))
    check("the launcher's own metronet.asi is known by its bytes: %s (%s)" % (MOD_VERSION, st[-1].split(" | ")[0]),
          "screen ready" in st[-1] and ("mod %s " % MOD_VERSION) in st[-1])
    with open(os.path.join(GAME, "metronet.asi"), "ab") as f:
        f.write(b"another build of the mod")
    st = states(drive("by-hand2", "until:update:20000;state;quit", repo=feed.url))
    check("another metronet.asi is 'of an unknown version': the launcher offers its own (%s)" % st[-1].split(" | ")[0],
          "screen update" in st[-1] and "mod ? " in st[-1])
    rc = headless("/uninstall", GAME, "by-hand-uninstall")
    left = [n for n in MOD_FILES + [LIST] if os.path.exists(os.path.join(GAME, n))]
    check("/uninstall takes such a mod out too, its own loader included (exit code %s, left: %s)" % (rc, left or "nothing"),
          rc == 0 and not left)
    check("the game's own steam_api64.dll is as it was", read(os.path.join(GAME, "steam_api64.dll")) == api)
    rc = headless("/uninstall", GAME, "by-hand-nothing")
    check("with no mod in the folder /uninstall says so (exit code 2: %s)" % rc, rc == 2 and "в папке нет мода" in log_of("by-hand-nothing"))


def s_steam(feed):
    print("steam: whether Steam runs on this PC")
    for want in (1, 0):
        st = states(drive("steam%d" % want, "until:ready:20000;state;quit", repo=feed.url, steam=want))
        check("told %d, the page has steam %d" % (want, want), ("steam %d " % want) in st[-1])
    real = b"steam.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"], capture_output=True).stdout.lower()
    st = states(drive("steam-real", "until:ready:20000;state;quit", repo=feed.url, steam=None))
    check("left to itself the launcher sees what this PC has: steam.exe %s" % ("runs" if real else "does not run"),
          ("steam %d " % (1 if real else 0)) in st[-1])


def s_window(feed):
    print("window: the hidden window draws itself and takes presses at their places")
    text = drive("window", "until:ready:20000;mouse:110,200;state;mouse:110,138;state;mouse:400,325;wait:300;mouse:110,386;wait:200;"
                 "mouse:866,196;wait:400;state;quit", repo=feed.url)
    st = states(text)
    check("the window's own target draws", "window: drawn" in text and "drawing FAILED" not in text)
    check("a press on the station СЕТЬ opens that page, on ИГРА brings the first one back",
          len(st) == 3 and " tab 1 " in st[0] and " tab 0 " in st[1])
    check("a press on the orange block is ИГРАТЬ (the game is not started by a script)", "] play" in text)
    check("on the station ЛАУНЧЕР a press on ПРОВЕРИТЬ asks the feed again", " tab 4 " in st[2] and text.count("check: asking") == 2)


# The game's own values for each row: (the launcher's name of the row, [what user.cfg holds for each value, in order]).
CHOICES = [
    ("fullscreen", [["r_fullscreen off"], ["r_fullscreen off"], ["r_fullscreen on"]]),  # window, frameless (metronet.ini), full
    ("vsync", [["r_vsync off"], ["r_vsync on"]]),
    ("quality", [["r_quality_level %d" % i] for i in range(4)]),
    ("ssaa", [["r_supersample 1.", "r_enum_ssaa 10"], ["r_supersample 0.5", "r_enum_ssaa 5"], ["r_supersample 2.", "r_enum_ssaa 20"],
              ["r_supersample 3.", "r_enum_ssaa 30"], ["r_supersample 4.", "r_enum_ssaa 40"]]),
    ("af", [["r_af_level 0"], ["r_af_level 1"]]),
    ("tess", [["r_dx11_tess 0", "r_tess_ss 0"], ["r_dx11_tess 1", "r_tess_ss 0"], ["r_dx11_tess 1", "r_tess_ss 1"],
              ["r_dx11_tess 1", "r_tess_ss 2"]]),
    ("blur", [["r_blur_level 0"], ["r_blur_level 1"]]),
]
SLIDERS = [  # (row, its line, [(where the knob is put, what the file then holds)])
    ("fov", "r_base_fov", [(0, "45."), (1, "90."), (0.5, "67.5"), (0.125, "50.625")]),
    ("gamma", "r_gamma", [(0, "0.5"), (1, "1.5"), (0.25, "0.75"), (0.5, "1.")]),
]


def walk(script_name, rows, feed, tab):
    """Every value of every row in turn, the file's own line read after each press; (ok, what was seen)."""
    script = "until:ready:20000;click:tab.%s" % tab
    for key, values in rows:
        for _ in values:
            script += ";click:g.%s.next" % key + "".join(";file:" + v.split(" ")[0] for v in values[0])
    text = drive(script_name, script + ";quit", repo=feed.url)
    seen = lines_of(text, "file")
    at, good, all_values = 0, True, {}
    for key, values in rows:
        start = next((i for i, v in enumerate(values) if all(cfg_line(USER_CFG, line.split(" ")[0]) == line for line in v)), 0)
        got = []
        for step in range(1, len(values) + 1):
            want = values[(start + step) % len(values)]
            got.append(seen[at:at + len(want)])
            good = good and seen[at:at + len(want)] == want
            at += len(want)
        all_values[key] = got
    return good and at == len(seen), all_values, text


def s_graphics(feed):
    print("graphics: the ГРАФИКА station writes the game's user.cfg line by line")
    fresh_user()
    ok, seen, text = walk("graphics", CHOICES, feed, "graphics")
    check("every value of every row goes into the file as the game writes it, and round again (%s)" %
          ", ".join("%s %d" % (k, len(v)) for k, v in seen.items()), ok)
    check("after the full round the file is the one it was, byte for byte", read(CFG) == USER_CFG)
    check("before the first change one copy was put beside it: user.cfg.metronet-backup", os.path.exists(CFG_BACKUP) and read(CFG_BACKUP) == USER_CFG)
    # the screen's sizes: what this monitor has, and the file's own
    text = drive("graphics-res0", "until:ready:20000;click:tab.graphics;cfg;quit", repo=feed.url)
    m = re.search(r"cfg: row res (\d+)/(\d+) changed 0 \| r_res_hor 1284 \| r_res_vert 722 \| 1284 × 722", text)
    check("the list of screens has the file's own size and the monitor's (%s of them)" % (m.group(2) if m else "?"), bool(m) and int(m.group(2)) >= 2)
    n = int(m.group(2)) if m else 1
    text = drive("graphics-res", "until:ready:20000;click:tab.graphics" + ";click:g.res.next;file:r_res_hor;file:r_res_vert" * n +
                 ";click:g.res.prev;file:r_res_hor;file:r_res_vert;quit", repo=feed.url)
    f = lines_of(text, "file")
    sizes = [(f[i], f[i + 1]) for i in range(0, len(f) - 1, 2)]
    said = re.findall(r"cfg res = r_res_hor (\d+) \| r_res_vert (\d+)", text)
    check("every size of the list goes into r_res_hor / r_res_vert, and the round ends at the file's own",
          len(sizes) == n + 1 and len(set(sizes[:n])) == n and sizes[n - 1] == ("r_res_hor 1284", "r_res_vert 722") and
          [("r_res_hor " + w, "r_res_vert " + h) for w, h in said] == sizes)
    # the sliders
    script = "until:ready:20000;click:tab.graphics"
    for key, name, stops in SLIDERS:
        script += "".join(";slide:%s:%s;file:%s" % (key, at, name) for at, _ in stops)
    text = drive("graphics-sliders", script + ";cfg;quit", repo=feed.url)
    want = ["%s %s" % (name, value) for _, name, stops in SLIDERS for _, value in stops]
    check("the knobs write what the game takes: the field of view 45 .. 90, gamma 0.5 .. 1.5 (%s)" % lines_of(text, "file"), lines_of(text, "file") == want)
    names = changed_names(USER_CFG, read(CFG))
    check("all that differs from the first file now is the screen's size (%s)" % sorted(names or ["?"]), names == {"r_res_hor", "r_res_vert"})
    check("the copy beside the file is still the first file: it is made once", read(CFG_BACKUP) == USER_CFG)
    text = drive("graphics-back", "until:ready:20000;click:tab.graphics;cfg;quit", repo=feed.url)
    check("a new start reads the file back", bool(re.search(r"cfg: row fov at 125 changed 0 \| r_base_fov 50\.625 \| 50,625", text)) and
          "cfg: row quality 1/4 changed 0 | r_quality_level 1 | Среднее" in text)


def s_controls(feed):
    print("controls: the УПРАВЛЕНИЕ station - the mouse's rows and the game's keys")
    fresh_user()
    steps = [  # (what is done, the lines of the file read after it, what they must be)
        ("click:bind.jump;keydown:0x4A:0x24:0", ["bind jump"], ["bind jump kJ"]),                              # a free key
        ("click:bind.forward;keydown:0x53:0x1F:0", ["bind forward", "bind back"], ["bind forward kS", "bind back kW"]),  # a taken one: they change places
        ("click:bind.use;xdown:1", ["bind use"], ["bind use mouse3"]),                                         # a side button of the mouse
        ("click:bind.use;rdown", ["bind use", "bind wpn_aim"], ["bind use mouse1", "bind wpn_aim mouse3"]),    # the right one, taken
        ("click:bind.crouch;click:bind.crouch", ["bind crouch", "bind wpn_fire"], ["bind crouch mouse0", "bind wpn_fire kLCONTROL"]),  # the left: on the same row
        ("click:bind.sprint;keydown:0x5B:0x5B:1;key:esc", ["bind sprint"], ["bind sprint kLSHIFT"]),           # a key the game has no name for; Esc
        ("click:bind.sprint;keydown:0xA3:0x1D:1", ["bind sprint"], ["bind sprint kRCONTROL"]),                 # a key of the extended set
        ("click:bind.jump.reset", ["bind jump"], ["bind jump kSPACE"]),                                        # back to the game's own
        ("click:bind.forward.reset", ["bind forward", "bind back"], ["bind forward kW", "bind back kS"]),
        ("click:keys.next;click:bind.wpn_reload;keydown:0x54:0x14:0", ["bind wpn_reload", "bind filter"], ["bind wpn_reload kT", "bind filter kR"]),
        ("slide:sens:0", ["sens"], ["sens 0.1"]),
        ("slide:sens:1", ["sens"], ["sens 2."]),
        ("slide:sens:0.5", ["sens"], ["sens 1.05"]),
        ("click:g.invert", ["invert_y_axis"], ["invert_y_axis on"]),
        ("slide:aim:0.5", ["aim_assist"], ["aim_assist 0.5"]),
        ("click:g.vibration.next", ["vibration"], ["vibration 3"]),
        ("click:g.vibration.next", ["vibration"], ["vibration 0"]),
        ("click:g.vibration.prev", ["vibration"], ["vibration 3"]),
    ]
    script = "until:ready:20000;click:tab.controls;cfg"
    for do, names, _ in steps:
        script += ";" + do + "".join(";file:" + n for n in names)
    text = drive("controls", script + ";cfg;quit", repo=feed.url)
    seen, at, bad = lines_of(text, "file"), 0, []
    for do, names, want in steps:
        if seen[at:at + len(want)] != want:
            bad.append("%s -> %s" % (do, seen[at:at + len(want)]))
        at += len(want)
    check("a key, a taken key, the mouse's buttons, a key of no name, the game's own key again, the mouse's rows: %d steps%s" %
          (len(steps), "" if not bad else " (" + "; ".join(bad) + ")"), not bad and at == len(seen))
    check("a key the game has no name for is not written and is said", "Эту клавишу игре не назначить" in text)
    first = [c for c in lines_of(text, "cfg") if c.startswith("bind ")][:len(BINDS)]
    check("the page lists every key of the file, the game's own ones marked so (%d rows)" % len(first),
          len(first) == len(BINDS) and all((" " + b + " changed 0 own 1 |") in (" " + c) for b, c in zip(BINDS, first)) and
          "cfg: bind forward kW changed 0 own 1 | Вперёд | W" in text and "cfg: bind special k3 changed 0 own 1 | special | 3" in text)
    names = changed_names(USER_CFG, read(CFG))
    want = {"bind use", "bind wpn_aim", "bind crouch", "bind wpn_fire", "bind sprint", "bind wpn_reload", "bind filter", "sens", "invert_y_axis",
            "aim_assist", "vibration"}
    check("only the lines of what was changed differ from the first file, the order is the same (%d lines)" % len(names or []), names == want)
    check("the copy beside the file is the first file", os.path.exists(CFG_BACKUP) and read(CFG_BACKUP) == USER_CFG)
    text = drive("controls-back", "until:ready:20000;click:tab.controls;cfg;quit", repo=feed.url)
    check("a new start reads the keys back: what differs from the game's own is marked",
          "cfg: bind use mouse1 changed 0 own 0 | Использовать | ПКМ" in text and "cfg: bind sprint kRCONTROL changed 0 own 0 | Бег | Правый Ctrl" in text and
          "cfg: bind jump kSPACE changed 0 own 1 | Прыжок | Пробел" in text and "cfg: row sens at 500 changed 0 | sens 1.05 | 1,05" in text)


def s_cfg_guard(feed):
    print("cfg-guard: no change while the game runs; no user.cfg - none is made")
    fresh_user()
    text = drive("cfg-running", "until:ready:20000;click:tab.graphics;click:g.quality.next;file:r_quality_level;slide:fov:1;file:r_base_fov;"
                 "click:tab.controls;click:bind.jump;keydown:0x4A:0x24:0;file:bind jump;quit", repo=feed.url, running=1)
    check("with the game running nothing is written (%s)" % lines_of(text, "file"),
          lines_of(text, "file") == ["r_quality_level 1", "r_base_fov 50.625", "bind jump kSPACE"] and read(CFG) == USER_CFG)
    check("... the launcher says why, and makes no copy", text.count("Игра запущена") >= 3 and text.count("NOT WRITTEN") >= 3 and not os.path.exists(CFG_BACKUP))
    real = drive("cfg-real", "until:ready:20000;click:tab.graphics;click:g.quality.next;file:r_quality_level;click:g.quality.prev;quit",
                 repo=feed.url, running=None)
    check("left to itself the launcher minds only a metro.exe of its own game folder (none runs from the lab's)",
          lines_of(real, "file") == ["r_quality_level 2"] and read(CFG) == USER_CFG)
    fresh_user(with_file=False)
    shot = os.path.join(WORK, "shots", "no-user-cfg.png")
    text = drive("cfg-none", "until:ready:20000;click:tab.graphics;cfg;click:g.quality.next;shot:%s;click:tab.controls;cfg;quit" % shot,
                 repo=feed.url, may_miss=True)
    check("without a user.cfg the rows cannot be pressed", lines_of(text, "cfg")[:1] == ["file 0 page 0/0 waits -1 path " + CFG] and
          "click g.quality.next: NOT ON THIS SCREEN" in text)
    check("... and no file is made", os.listdir(USER) == [])
    fresh_user()


def s_no_release():
    print("no-release: nothing is published yet - a quiet state, not an error")
    for mode in ("norepo", "empty"):
        feed = launcher_feed.Feed(os.path.join(WORK, "feed-" + mode), mode)
        name = "no-release-" + mode
        text = drive(name, "until:ready:20000;state;shot:%s;quit" % os.path.join(WORK, "shots", name + ".png"), repo=feed.url)
        st = states(text)
        check("%s: ready, 'no release yet', the mod of the launcher stays (%s)" % (mode, st[-1].split(" | ")[0]),
              "screen ready" in st[-1] and "norelease 1" in st[-1] and "check: no release yet" in text)
        check("%s: no problem page, no 'offline' page" % mode, "problem" not in text and "offline" not in text)
        log = os.path.join(WORK, name + "-check.log")
        rc = launcher(["--check", "--repo", feed.url, "--settings", os.path.join(WORK, name + ".ini"), "--out", log])
        check("%s: --check says the same (exit code 4: %s)" % (mode, rc), rc == 4 and "norelease 1" in log_of(name + "-check"))
        feed.stop()


def s_no_answer():
    print("no-answer: the server does not answer at all")
    text = drive("no-answer", "until:offline:40000;state;click:primary;wait:300;quit", repo="http://127.0.0.1:9/r")
    st = states(text)
    check("the mod stays, ИГРАТЬ is there (%s)" % st[-1].split(" | ")[0], "screen offline" in st[-1] and "] play" in text and "problem" not in text)


def reset_mod():
    """The stand-in game with the launcher's own mod in it again."""
    shutil.copyfile(BUILT, MINE)
    assert headless("/install", GAME, "reset") == 0
    assert installed_version(GAME) == MOD_VERSION


def s_update():
    print("update: a newer mod comes inside a newer launcher from the local feed")
    newer = "0.%d.%d" % (int(MOD_VERSION.split(".")[1]) + 1, 0)
    made = launcher_newer.main(BUILT, FEED, newer, "1.2.1")
    feed = launcher_feed.Feed(FEED, "release")
    fresh_app()
    ini = read(os.path.join(GAME, "metronet.ini"))
    text = drive("update", "until:update:20000;state;click:primary;until:ready:60000;state;quit", repo=feed.url)
    st = states(text)
    check("the launcher sees %s on the feed (%s)" % (newer, st[0].split(" | ")[0]),
          len(st) == 2 and "screen update" in st[0] and ("github %s" % newer) in st[0])
    check("it downloads the release's launcher and checks it", ("job: install %s from GitHub" % newer) in text and "downloaded 100%" in text)
    check("the mod in the game folder is %s now (%s)" % (newer, installed_version(GAME)), installed_version(GAME) == newer)
    check("then: ready on %s" % newer, len(st) == 2 and "screen ready" in st[1] and ("mod %s " % newer) in st[1])
    check("the launcher's own file became the release's (1.2.1)", sha(MINE) == made["sha256"] and "launcher replaced by 1.2.1" in text)
    check("the player's metronet.ini was not touched by the update", read(os.path.join(GAME, "metronet.ini")) == ini)
    feed.stop()
    return newer


def s_old_settings():
    print("old-settings: the settings 1.0.0 wrote (auto_install = 0, nobody chose it): a newer mod installs by itself")
    newer = "0.%d.%d" % (int(MOD_VERSION.split(".")[1]) + 1, 0)
    launcher_newer.main(BUILT, FEED, newer, "1.2.1")
    feed = launcher_feed.Feed(FEED, "release")
    fresh_app()
    reset_mod()
    text = drive("old-settings", "until:ready:60000;state;quit", repo=feed.url, old_settings=True)
    st = states(text)
    check("no press: the launcher installs %s by itself (%s)" % (newer, installed_version(GAME)),
          installed_version(GAME) == newer and ("job: install %s from GitHub" % newer) in text and "click:" not in text)
    check("then: ready on %s" % newer, bool(st) and "screen ready" in st[-1] and ("mod %s " % newer) in st[-1])
    ini = io.open(os.path.join(WORK, "old-settings.ini"), encoding="utf-16").read()
    check("its settings say so from now on (settings=2, auto_install=1)", "settings=2" in ini and "auto_install=1" in ini)
    feed.stop()


def s_bad_sum(newer):
    print("bad-sum: a file that is not the release's is not used")
    doc = io.open(os.path.join(FEED, "metronet-latest.json"), encoding="utf-8").read()
    real = re.search(r'"sha256": "([0-9a-f]{64})"', doc).group(1)
    io.open(os.path.join(FEED, "metronet-latest.json"), "w", encoding="utf-8", newline="\n").write(
        doc.replace(real, ("0" if real[0] != "0" else "1") + real[1:]))
    feed = launcher_feed.Feed(FEED, "release")
    fresh_app()
    reset_mod()
    shutil.rmtree(os.path.join(WORK, "cache"))
    text = drive("bad-sum", "until:update:20000;click:primary;until:problem:60000;state;quit", repo=feed.url)
    check("the page says the file is damaged", "problem: Файл повреждён" in text)
    check("the mod in the game folder stays %s (%s)" % (MOD_VERSION, installed_version(GAME)), installed_version(GAME) == MOD_VERSION)
    check("the launcher's own file stays", sha(MINE) == sha(BUILT))
    check("the downloaded file is not kept", not [n for n in os.listdir(os.path.join(WORK, "cache")) if n.endswith(".exe")])
    feed.stop()


def s_self_update():
    print("self-update: a newer launcher with the same mod takes this one's place at the start")
    made = launcher_newer.main(BUILT, FEED, MOD_VERSION, "1.2.1")
    feed = launcher_feed.Feed(FEED, "release")
    fresh_app()
    reset_mod()
    text = drive("self-update", "wait:60000;quit", repo=feed.url, auto_self=True, next_script="until:ready:30000;wait:1500;state;quit",
                 timeout=120, ends=2)
    check("the launcher downloads the newer launcher by itself", "job: a newer launcher, 1.2.1" in text and "launcher replaced by 1.2.1" in text)
    check("its file is the release's now", sha(MINE) == made["sha256"])
    check("the newer launcher is started in its place and goes no further",
          "restart: the newer launcher is started" in text and "started by launcher 1.2.0 in its place" in text and
          text.count("job: a newer launcher") == 1)
    st = states(text)
    check("the mod was not touched: ready on %s (%s)" % (MOD_VERSION, st[-1].split(" | ")[0]),
          "screen ready" in st[-1] and installed_version(GAME) == MOD_VERSION)
    time.sleep(1.0)
    check("the old launcher's file is removed", not os.path.exists(MINE + ".old"))
    feed.stop()


def s_game_open(feed):
    print("game-open: the game holds the mod's file - the launcher waits for it to close")
    fresh_app()
    reset_mod()
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wt.HANDLE
    k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
    k32.CloseHandle.argtypes = [wt.HANDLE]
    held = k32.CreateFileW(os.path.join(GAME, "metronet.asi"), 0x80000000, 1, None, 3, 0, None)  # read, others may only read
    assert held and held != wt.HANDLE(-1).value, ctypes.get_last_error()
    threading.Timer(6.0, lambda: k32.CloseHandle(held)).start()
    text = drive("game-open", "until:ready:20000;click:tab.launcher;click:reinstall;until:running:20000;state;until:ready:40000;state;quit",
                 repo=feed.url)
    st = states(text)
    check("while the file is held the page says the game is open (%s)" % st[0].split(" | ")[0], len(st) == 2 and "screen running" in st[0])
    check("once it is let go the install goes on by itself", len(st) == 2 and "screen ready" in st[1] and "job done: step 0" in text)


def s_pictures():
    print("pictures: the stations as PNG files, drawn without a window")
    for page in ("main", "net", "graphics", "controls", "launcher", "install", "update", "wrongbuild", "norelease", "graphics-nofile"):
        out = os.path.join(WORK, "shots", page + ".png")
        rc = launcher(["--shot", page, out, "--steam", "1"])
        d = read(out) if os.path.exists(out) else b""
        size = struct.unpack(">II", d[16:24]) if d[:8] == b"\x89PNG\r\n\x1a\n" else None
        check("%s: a PNG of 960 x 600 (%s)" % (page, size), rc == 0 and size == (960, 600))


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")  # (the checks' lines name buttons in Russian)
    only = sys.argv[2:] if len(sys.argv) > 2 and sys.argv[1] == "only" else None
    if not os.path.exists(BUILT):
        print("build first: %s is missing" % BUILT)
        return 2
    print("launcher lab: %s, the mod %s inside; everything under %s" % (BUILT, MOD_VERSION, WORK))
    read_inside()
    prepare()

    def wanted(name):
        return only is None or name in only

    quiet = launcher_feed.Feed(os.path.join(WORK, "feed-quiet"), "norepo")  # "nothing published yet": the feed of most runs
    if wanted("where"):
        s_where()
    s_install()  # (always: the scenarios below build on the stand-in game with the mod in it)
    if wanted("keep-ini"):
        s_keep_ini()
    if wanted("settings"):
        s_settings(quiet)
    if wanted("bare-ini"):
        s_bare_ini(quiet)
    if wanted("uninstall"):
        s_uninstall(quiet)
    if wanted("window-install"):
        s_window_install(quiet)
    if wanted("wrong-build"):
        s_wrong_build(quiet)
    if wanted("pick"):
        reset_mod()
        s_pick(quiet)
    if wanted("by-hand"):
        s_by_hand(quiet)
    if wanted("steam"):
        reset_mod()
        s_steam(quiet)
    if wanted("window"):
        reset_mod()
        s_window(quiet)
    if wanted("graphics"):
        reset_mod()
        s_graphics(quiet)
    if wanted("controls"):
        reset_mod()
        s_controls(quiet)
    if wanted("cfg-guard"):
        reset_mod()
        s_cfg_guard(quiet)
    if wanted("no-release"):
        reset_mod()
        s_no_release()
    if wanted("no-answer"):
        reset_mod()
        s_no_answer()
    if wanted("update") or wanted("bad-sum"):
        fresh_app()
        reset_mod()
        newer = s_update()
        if wanted("bad-sum"):
            s_bad_sum(newer)
    if wanted("old-settings"):
        s_old_settings()
    if wanted("self-update"):
        s_self_update()
    if wanted("game-open"):
        s_game_open(quiet)
    if wanted("pictures"):
        fresh_app()
        s_pictures()
    quiet.stop()
    print("launcher lab: %d checks, %d failed" % (checks, failed))
    return failed


if __name__ == "__main__":
    sys.exit(main())
