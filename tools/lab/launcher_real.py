"""launcher_real.py <old launcher.exe> [--proxy host:port]

The release on the real GitHub, as a player who has an older launcher meets it: a stand-in game folder (a metro.exe
of the build the mod knows, nothing else of the game) gets the old launcher's mod, then the old launcher is started
with the settings an old launcher leaves (no "settings" line, auto_install = 0) and asks the real repository. It
must take the release's launcher in its own place by itself, start it, and that one must install the release's mod
without a press. Nothing of the player's own game, settings or launcher is touched: all of it lives in
build/launcher-lab/real.
"""
import io, json, os, shutil, subprocess, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_lab as L  # noqa: E402

a = sys.argv[1:]
proxy = None
if "--proxy" in a:
    i = a.index("--proxy")
    proxy = a[i + 1]
    del a[i:i + 2]
old = os.path.abspath(a[0])
W = os.path.join(L.REPO, "build", "launcher-lab", "real")
shutil.rmtree(W, ignore_errors=True)
game, app, user, cache = (os.path.join(W, n) for n in ("game", "app", "user", "cache"))
for d in (game, app, user, cache):
    os.makedirs(d)
L.fake_exe(os.path.join(game, "metro.exe"), L.STAMP, L.IMAGE)
mine = os.path.join(app, "metronet-launcher.exe")
shutil.copyfile(old, mine)


def run(args, timeout=120):
    p = subprocess.Popen([mine] + args, cwd=app)
    try:
        return p.wait(timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        return None


def installed():
    return L.installed_version(game) or None


rc = run(["/install", game, "/quiet", "--out", os.path.join(W, "install.log")])
before = installed()
print("the old launcher put its mod in: %s (exit %s)" % (before, rc))

feed = "https://github.com/int3re/METRO-2033-REDUX-COOP/releases/latest/download/metronet-latest.json"
opener = urllib.request.build_opener(urllib.request.ProxyHandler({"https": "http://" + proxy} if proxy else {}))
rel = json.loads(opener.open(feed, timeout=60).read().decode("utf-8"))
print("the real GitHub says: mod %s, launcher %s" % (rel["version"], rel["launcher"]["version"]))

ini, log = os.path.join(W, "launcher.ini"), os.path.join(W, "run.log")
io.open(ini, "w", encoding="utf-16").write(
    "[launcher]\ngame_dir=%s\nauto_check=1\nauto_self=1\nauto_install=0\nclose_on_play=0\n%s" %
    (game, ("proxy=2\nproxy_addr=%s\n" % proxy) if proxy else "proxy=0\n"))
# (a scripted launcher never asks the real GitHub unless it is told its address: --repo)
run(["--repo", "https://github.com/int3re/METRO-2033-REDUX-COOP", "--drive", "wait:60000;quit", "--drive-next", "until:ready:90000;wait:1500;state;quit", "--settings", ini, "--cache", cache,
     "--out", log, "--userdir", user, "--steam", "1", "--game-running", "0"], timeout=200)
until = time.time() + 200
while (not os.path.exists(log) or io.open(log, encoding="utf-8", errors="replace").read().count("] exit") < 2) and time.time() < until:
    time.sleep(0.5)
text = io.open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
after = installed()
checks = [
    ("the old launcher took the release's launcher %s in its own place" % rel["launcher"]["version"],
     ("launcher replaced by %s" % rel["launcher"]["version"]) in text and L.sha(mine) == rel["launcher"]["sha256"]),
    ("the new launcher was started in its place", "restart: the newer launcher is started" in text),
    ("it installed the release's mod %s without a press (%s -> %s)" % (rel["version"], before, after), after == rel["version"]),
    ("its settings say so from now on", "settings=2" in io.open(ini, encoding="utf-16").read()),
]
failed = 0
for what, ok in checks:
    print("%-4s %s" % ("ok" if ok else "FAIL", what))
    failed += not ok
print("---- the launchers' log")
for line in text.splitlines():
    if any(k in line for k in ("job", "launcher", "restart", "screen", "install", "github", "GitHub", "exit", "problem", "FAIL")):
        print("  " + line[:200])
print("RESULT: %s" % ("PASS (%d checks)" % len(checks) if not failed else "FAIL (%d of %d)" % (failed, len(checks))))
sys.exit(1 if failed else 0)
