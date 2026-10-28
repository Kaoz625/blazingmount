#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bm-console.py - BlazingMount from any computer: console state + REMOTE WAKE, console profiles, network sources (FTP / SMB / WebDAV).

    bm-console.py state [PHAT5|DIGI5|all]            awake / rest mode / silent (Remote Play discovery: read-only, never wakes) + payload + profile
    bm-console.py wake <console> [--wait 90]         wake a console that is in REST MODE (the Remote Play / DDP wake packet with the registered
                                                     credential of the Game Lab's pairing); refuses an awake or silent console; never puts one to rest
    bm-console.py profile <console> [show]           this console's profile (stored on the console: /data/blazing/profile.conf)
    bm-console.py profile <console> set [--name N] [--target auto|internal|m2] [--source NAME] [--user U]
    bm-console.py src <console> list|test NAME|ls NAME [PATH] [--all]|del NAME
    bm-console.py src <console> add NAME KIND HOST[:PORT] [PATH] [--user U (--pass-env VAR | --pass-file FILE | --ask)]
          KIND = ftp (PATH /dir) | smb (PATH share[/dir]) | webdav (PATH /dav); the login is saved ON THE CONSOLE (mode 0600), never printed
    bm-console.py install <console> NAME PATH        install a .pkg (split X.pkg.N sets as X.pkg) from a source; live %, MB/s, ETA, Ctrl-C cancels

<console> = PHAT5 | DIGI5 (found by identity with blazing-ps5 ps5where) or an IPv4 address. --api-port / --ddp-port exist for the tests.
The wake credential never leaves the computer that holds the pairing profile: this one (~/.blazing/ps5-remote/profile.json, or $BM_RP_PROFILE),
or else mac2 over ssh (the Game Lab's pyremoteplay host, $GAMELAB_RP_HOST). The packet is sent from there; nothing secret is printed.
Exit: 0 ok, 1 failed, 2 usage, 3 console unreachable / refused, 4 not found.
"""
import argparse, base64, getpass, importlib.util, json, os, re, select, socket, subprocess, sys, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DDP_PORT = 9302
SRCH = b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"
API_PORT = 8094
CONSOLES = {"PHAT5": "5C9666D8A2CB", "DIGI5": "9C37CB636602"}      # Remote Play host-id = the wired MAC (DEVICES.md)
RP_HOST = os.environ.get("GAMELAB_RP_HOST", "mac2")
RP_PROFILE = "~/.blazing/ps5-remote/profile.json"


# ------------------------------------------------------------------ finding a console
def _ps5where():
    for d in (os.environ.get("BLAZING_PS5", ""), os.path.expanduser("~/NYCT/blazing-ps5")):
        p = os.path.join(d, "tools", "ps5where.py") if d else ""
        if p and os.path.exists(p):
            spec = importlib.util.spec_from_file_location("ps5where", p)
            m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
            return m
    return None


def ddp_search(ip, port=DDP_PORT, timeout=1.5):
    """One Remote Play discovery answer from this address ({'status': 200|620, 'host-id': ..., ...}) or None. Read-only."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        t0 = time.monotonic(); nxt = 0.0
        while time.monotonic() - t0 < timeout:
            if time.monotonic() - t0 >= nxt:
                s.sendto(SRCH, (ip, port)); nxt += 0.5
            r, _, _ = select.select([s], [], [], 0.05)
            if r:
                data, _ = s.recvfrom(4096)
                lines = data.decode("utf-8", "replace").replace("\r", "").split("\n")
                m = re.match(r"HTTP/1\.[01] (\d{3})", lines[0]) if lines else None
                if m:
                    rec = {"status": int(m.group(1))}
                    for ln in lines[1:]:
                        if ":" in ln:
                            k, v = ln.split(":", 1); rec[k.strip().lower()] = v.strip()
                    return rec
    except OSError:
        return None
    finally:
        s.close()
    return None


def power_of(rec):
    if not rec:
        return "silent"
    return "awake" if rec["status"] == 200 else "rest mode" if rec["status"] == 620 else "status %s" % rec["status"]


def locate(console, ddp_port=DDP_PORT):
    """(name, ip, ddp_record or None). A name is found by identity (ps5where); an IPv4 address is taken as is."""
    if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", console):
        rec = ddp_search(console, ddp_port)
        hid = (rec or {}).get("host-id", "").upper()
        name = next((n for n, h in CONSOLES.items() if h == hid), console)
        return name, console, rec
    name = console.upper()
    pw = _ps5where()
    if pw:
        c = pw.canon(console)
        if not c:
            sys.exit("not a console name: %s (PHAT5, DIGI5 or an address)" % console)
        r = pw.find(c)
        if r:
            return c, r["host"], ddp_search(r["host"], ddp_port)
        sys.exit("%s was not found (ps5-where %s says why)" % (c, c))
    sys.exit("ps5where.py not found (~/NYCT/blazing-ps5/tools): give the console's address")


# ------------------------------------------------------------------ the console's API
def api(ip, method, path, timeout=15):
    req = urllib.request.Request("http://%s:%d%s" % (ip, API_PORT, path), method=method, headers={"X-BM-Request": "1"})
    try:
        r = urllib.request.urlopen(req, timeout=timeout); return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}
    except (OSError, ValueError) as e:
        return -1, {"error": str(e)}


def q(**kw):
    return urllib.parse.urlencode({k: v for k, v in kw.items() if v is not None})


# ------------------------------------------------------------------ state
def cmd_state(a):
    names = ["PHAT5", "DIGI5"] if a.console in ("all", None) else [a.console]
    rc = 0
    for n in names:
        try:
            name, ip, rec = locate(n, a.ddp_port)
        except SystemExit as e:
            print("%-6s %s" % (n, e)); rc = 4; continue
        extra = ""
        if rec and rec["status"] == 200:
            code, st = api(ip, "GET", "/api/status", 4)
            code2, pf = api(ip, "GET", "/api/profile", 4)
            if code == 200:
                extra = "  payload %s" % st.get("version")
            if code2 == 200 and (pf.get("name") or pf.get("user")):
                extra += "  profile: %s (target %s, user %s, source %s)" % (pf.get("name") or "-", pf.get("install_target"), pf.get("user") or "-", pf.get("default_source") or "-")
            app = rec.get("running-app-name") or rec.get("running-app-titleid") or ""
            extra += ("  playing: %s" % app) if app else ""
        print("%-6s %-15s %-10s FW %s%s" % (name, ip, power_of(rec), (rec or {}).get("system-version", "?")[:2] + "." + (rec or {}).get("system-version", "?")[2:4] if rec else "?", extra))
    return rc


# ------------------------------------------------------------------ wake
# Runs where the pairing profile is (here, or mac2 over ssh). Reads the job on stdin (JSON), prints ONE 'RESULT {...}' line, never a key.
WAKE_PY = r'''
import json, os, socket, sys, time
job = json.loads(sys.stdin.read())
path = os.path.expanduser(os.environ.get("BM_RP_PROFILE") or "~/.blazing/ps5-remote/profile.json")
res = {"sent": False}
try:
    prof = json.load(open(path))
except Exception as e:
    print("RESULT " + json.dumps({"sent": False, "error": "no pairing profile (%s): %s" % (path, type(e).__name__)})); sys.exit(0)
hid, key, who = job["host_id"], None, None
for user, p in prof.items():
    d = ((p.get("hosts") or {}).get(hid) or {}).get("data") or {}
    if d.get("RegistKey") and (not job.get("user") or job["user"] == user):
        key, who = d["RegistKey"], user; break
if not key:
    print("RESULT " + json.dumps({"sent": False, "error": "this console is not paired in the profile (tools/gamelab/pair.py register)"})); sys.exit(0)
cred = str(int.from_bytes(bytes.fromhex(bytes.fromhex(key).decode()), "big"))
msg = ("WAKEUP * HTTP/1.1\nuser-credential:%s\nclient-type:vr\nauth-type:R\nmodel:w\napp-type:r\ndevice-discovery-protocol-version:00030010\n" % cred)
if job.get("dry_run"):
    print("RESULT " + json.dumps({"sent": False, "dry_run": True, "user": who, "packet_lines": msg.count("\n"), "credential_digits": len(cred)})); sys.exit(0)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(("", 9303))                                  # the port a PS5 expects
except OSError:
    pass
for i in range(3):
    s.sendto(msg.encode(), (job["ip"], int(job["port"]))); time.sleep(0.4)
s.close()
print("RESULT " + json.dumps({"sent": True, "user": who}))
'''


def run_wake_py(job):
    """Send the wake packet from the computer that holds the pairing profile. {'sent': bool, ...}"""
    local = os.path.exists(os.path.expanduser(os.environ.get("BM_RP_PROFILE") or RP_PROFILE))
    boot = "import sys,base64;exec(base64.b64decode(sys.argv[1]))"                  # the sender travels as an argument, the job on stdin
    enc = base64.b64encode(WAKE_PY.encode()).decode()
    cmd = [sys.executable, "-I", "-c", boot, enc] if local else ["ssh", "-o", "ConnectTimeout=10", "-o", "BatchMode=yes", RP_HOST, "python3 -I -c '%s' %s" % (boot, enc)]
    try:
        r = subprocess.run(cmd, input=json.dumps(job), capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"sent": False, "error": "could not run the sender (%s)" % type(e).__name__}
    for ln in r.stdout.splitlines():
        if ln.startswith("RESULT "):
            return json.loads(ln[7:])
    return {"sent": False, "error": "the sender said nothing (%s)" % (r.stderr.strip().splitlines() or ["?"])[-1][:120]}


def cmd_wake(a):
    name, ip, rec = locate(a.console, a.ddp_port)
    hid = (rec or {}).get("host-id", "").upper() or CONSOLES.get(name, "")
    st = power_of(rec)
    print("%s at %s: %s" % (name, ip, st))
    if st == "awake":
        print("already awake - nothing sent"); return 0
    if st != "rest mode" and not a.force_send:
        print("not in rest mode (%s): nothing sent (--force-send sends anyway)" % st); return 3
    if not hid:
        print("unknown host-id: cannot pick the credential"); return 1
    r = run_wake_py({"ip": ip, "port": a.ddp_port, "host_id": hid, "user": a.user, "dry_run": a.dry_run})
    if r.get("dry_run"):
        print("dry run: credential for %s found (user %s), packet of %d lines built; nothing sent" % (name, r.get("user"), r.get("packet_lines"))); return 0
    if not r.get("sent"):
        print("wake NOT sent: %s" % r.get("error")); return 1
    print("wake packet sent (3x, credential of '%s'); waiting up to %d s for %s ..." % (r.get("user"), a.wait, name))
    t0 = time.monotonic()
    while time.monotonic() - t0 < a.wait:
        rec = ddp_search(ip, a.ddp_port, 1.0)
        if rec and rec["status"] == 200:
            print("%s is awake after %d s" % (name, round(time.monotonic() - t0))); return 0
        time.sleep(1.0)
    print("%s did not wake within %d s (still %s)" % (name, a.wait, power_of(ddp_search(ip, a.ddp_port, 1.0)))); return 1


# ------------------------------------------------------------------ profile + sources
def cmd_profile(a):
    name, ip, rec = locate(a.console, a.ddp_port)
    if rec is not None and rec["status"] != 200:
        print("%s is %s: its profile is on the console, wake it first (bm-console.py wake %s)" % (name, power_of(rec), name)); return 3
    if a.action == "set":
        qs = q(name=a.name, install_target=a.target, default_source=a.source, user=a.user)
        if not qs:
            print("nothing to set (--name --target --source --user)"); return 2
        code, j = api(ip, "POST", "/api/profile/set?" + qs)
    else:
        code, j = api(ip, "GET", "/api/profile")
    if code != 200:
        print("%s: %s (%s)" % (name, j.get("error", "no answer"), code)); return 3
    print("%s profile: name=%s  target=%s (homebrew_dir %s%s)  default source=%s  user=%s  payload %s" % (
        name, j.get("name") or "-", j.get("install_target"), j.get("homebrew_dir"), "" if j.get("target_in_effect") else ", not yet applied",
        j.get("default_source") or "-", j.get("user") or "-", j.get("version")))
    return 0


def fmt_bytes(n):
    for u in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024 or u == "TB":
            return "%.1f %s" % (n, u) if u != "B" else "%d B" % n
        n /= 1024.0


def cmd_src(a):
    name, ip, rec = locate(a.console, a.ddp_port)
    if rec is not None and rec["status"] != 200:
        print("%s is %s: wake it first" % (name, power_of(rec))); return 3
    act = a.action
    if act == "list":
        code, j = api(ip, "GET", "/api/src")
        if code != 200:
            print("no answer (%s) - blazingmountd >= 2.12 needed" % code); return 3
        for s in j.get("sources", []):
            print("%-14s %-7s %s:%d  %s  user %s%s" % (s["name"], s["kind"], s["host"], s["port"], s["path"] or "/", s["user"] or "-", " (login saved)" if s["has_pass"] else ""))
        if not j.get("sources"):
            print("no sources yet (bm-console.py src %s add ...)%s" % (a.console, "" if j.get("smb") else "  [this payload has no SMB support]"))
        return 0
    if act == "add":
        if len(a.args) < 3:
            print("src add NAME KIND HOST[:PORT] [PATH]"); return 2
        nm, kind, hp = a.args[:3]; path = a.args[3] if len(a.args) > 3 else ("" if kind == "smb" else "/")
        host, _, port = hp.partition(":")
        pw = os.environ.get(a.pass_env, "") if a.pass_env else open(os.path.expanduser(a.pass_file)).read().strip("\n") if a.pass_file else getpass.getpass("password: ") if a.ask else ""
        code, j = api(ip, "POST", "/api/src/add?" + q(name=nm, kind=kind, host=host, port=port or None, path=path, user=a.user or None, **({"pass": pw} if pw else {})))
        pw = ""
        print("source %s saved on %s" % (nm, name) if code == 200 else "refused: %s" % j.get("error", code)); return 0 if code == 200 else 1
    if act in ("del", "test", "ls"):
        if not a.args:
            print("src %s NAME" % act); return 2
        if act == "del":
            code, j = api(ip, "POST", "/api/src/del?" + q(name=a.args[0])); print("removed" if code == 200 else "no such source"); return 0 if code == 200 else 1
        code, j = api(ip, "GET", "/api/src/%s?" % act + q(name=a.args[0], path=a.args[1] if len(a.args) > 1 else None, all=1 if a.all else None), 30)
        if not j.get("ok"):
            print("failed: %s" % j.get("error", code)); return 1
        if act == "test":
            print("ok: %d entries in the base folder (%d ms)" % (len(j["entries"]), j.get("ms", 0))); return 0
        for e in j["entries"]:
            print("%-5s %12s  %s%s" % ("dir" if e["dir"] else "pkg" if e["pkg"] else "image" if e["image"] else "file", "" if e["dir"] else fmt_bytes(e["size"]), e["name"] + ("/" if e["dir"] else ""),
                                       "  (%d parts)" % e["parts"] if e["parts"] > 1 else ""))
        return 0
    print("src list|add|ls|test|del"); return 2


def cmd_install(a):
    name, ip, rec = locate(a.console, a.ddp_port)
    if rec is not None and rec["status"] != 200:
        print("%s is %s: wake it first" % (name, power_of(rec))); return 3
    spec = importlib.util.spec_from_file_location("install_pkg", os.path.join(HERE, "install-pkg.py"))
    ip_mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(ip_mod); ip_mod.API_PORT = API_PORT
    code, j = api(ip, "POST", "/api/install?" + q(src=a.name, path=a.path))
    if code != 200:
        print("refused (%s): %s" % (code, j.get("error"))); return 3
    rc, j = ip_mod.follow(ip)
    print({0: "installed", 1: "FAILED: %s" % j.get("error"), 2: "cancelled"}[rc], j.get("title_id") or "")
    return rc


def main(argv=None):
    global API_PORT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--api-port", type=int, default=API_PORT, help=argparse.SUPPRESS)
    ap.add_argument("--ddp-port", type=int, default=DDP_PORT, help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("state"); p.add_argument("console", nargs="?", default="all"); p.set_defaults(fn=cmd_state)
    p = sub.add_parser("wake"); p.add_argument("console"); p.add_argument("--wait", type=int, default=90); p.add_argument("--user"); p.add_argument("--dry-run", action="store_true")
    p.add_argument("--force-send", action="store_true", help="send even when the console does not answer discovery as rest mode"); p.set_defaults(fn=cmd_wake)
    p = sub.add_parser("profile"); p.add_argument("console"); p.add_argument("action", nargs="?", default="show", choices=["show", "set"])
    p.add_argument("--name"); p.add_argument("--target", choices=["auto", "internal", "m2"]); p.add_argument("--source"); p.add_argument("--user"); p.set_defaults(fn=cmd_profile)
    p = sub.add_parser("src"); p.add_argument("console"); p.add_argument("action"); p.add_argument("args", nargs="*"); p.add_argument("--all", action="store_true")
    p.add_argument("--user"); p.add_argument("--pass-env"); p.add_argument("--pass-file"); p.add_argument("--ask", action="store_true"); p.set_defaults(fn=cmd_src)
    p = sub.add_parser("install"); p.add_argument("console"); p.add_argument("name"); p.add_argument("path"); p.set_defaults(fn=cmd_install)
    a = ap.parse_args(argv)
    API_PORT = a.api_port
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
