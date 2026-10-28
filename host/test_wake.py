#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""test_wake.py - host/bm-console.py's remote wake + state, against a FAKE console on 127.0.0.1 (UDP discovery answers 620 in rest mode,
200 awake; a WAKEUP packet with the right credential wakes it) and a fake pairing profile. No real console is touched.
Checks: state reads rest/awake/silent; the packet is the Remote Play wake packet with the profile's credential (and NOT anyone else's);
an awake console gets nothing; a silent one is refused; an unpaired one is refused; --dry-run sends nothing; the credential is never printed;
the console wakes and the tool reports it.
"""
import json, os, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "bm-console.py")
HID = "5C9666D8A2CB"                      # PHAT5's host-id
KEY = "3031303230333034"                  # RegistKey: hex of the ascii hex of 01 02 03 04
CRED = str(0x01020304)
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += 0 if ok else 1


class Fake:
    def __init__(self, state):
        self.state, self.wakes, self.srch = state, [], 0
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self.s.bind(("127.0.0.1", 0)); self.port = self.s.getsockname()[1]
        self.wake_at = None; threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                data, addr = self.s.recvfrom(4096)
            except OSError:
                return
            t = data.decode()
            if self.wake_at and time.time() >= self.wake_at:
                self.state = "awake"
            if t.startswith("SRCH"):
                self.srch += 1
                code = "200 Ok" if self.state == "awake" else "620 Server Standby"
                self.s.sendto(("HTTP/1.1 %s\nhost-id:%s\nhost-type:PS5\nhost-name:PS5-620\nhost-request-port:997\nsystem-version:10200006\ndevice-discovery-protocol-version:00030010\n" % (code, HID)).encode(), addr)
            elif t.startswith("WAKEUP"):
                self.wakes.append(t)
                if "user-credential:%s\n" % CRED in t and self.state != "awake" and not self.wake_at:
                    self.wake_at = time.time() + 1.5


def run(*args, env=None):
    e = dict(os.environ, **(env or {}))
    r = subprocess.run([sys.executable, "-I", TOOL] + list(args), capture_output=True, text=True, timeout=90, env=e)
    return r.returncode, r.stdout + r.stderr


def main():
    tmp = tempfile.mkdtemp(prefix="bm-wake-")
    prof = os.path.join(tmp, "profile.json")
    json.dump({"blazing": {"id": "x", "hosts": {HID: {"data": {"RegistKey": KEY, "RP-Key": "00" * 16}, "type": "PS5"}}}}, open(prof, "w"))
    env = {"BM_RP_PROFILE": prof, "GAMELAB_RP_HOST": "no-such-host.invalid"}          # never ssh to the real mac2 from a test
    rest, awake = Fake("rest"), Fake("awake")
    silent_port = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); silent_port.bind(("127.0.0.1", 0)); sp = silent_port.getsockname()[1]   # nobody answers

    rc, out = run("--ddp-port", str(rest.port), "state", "127.0.0.1")
    check(rc == 0 and "rest mode" in out and "PHAT5" in out, "state: rest mode, identified by host-id as PHAT5: %s" % out.strip())
    rc, out = run("--ddp-port", str(awake.port), "state", "127.0.0.1")
    check("awake" in out, "state: awake: %s" % out.strip())
    rc, out = run("--ddp-port", str(sp), "state", "127.0.0.1")
    check("silent" in out, "state: silent: %s" % out.strip())

    rc, out = run("--ddp-port", str(awake.port), "wake", "127.0.0.1", env=env)
    check(rc == 0 and "already awake" in out and not awake.wakes, "wake: an awake console gets NO packet (%d sent)" % len(awake.wakes))
    rc, out = run("--ddp-port", str(sp), "wake", "127.0.0.1", env=env)
    check(rc == 3 and "not in rest mode" in out, "wake: a silent console is refused: %s" % out.strip().splitlines()[-1])
    rc, out = run("--ddp-port", str(rest.port), "wake", "127.0.0.1", "--dry-run", env=env)
    check(rc == 0 and "dry run" in out and not rest.wakes, "wake --dry-run: credential found, nothing sent (%d packets)" % len(rest.wakes))
    # unpaired: a profile without this console's host-id
    prof2 = os.path.join(tmp, "p2.json"); json.dump({"blazing": {"id": "x", "hosts": {"AAAAAAAAAAAA": {"data": {"RegistKey": KEY}}}}}, open(prof2, "w"))
    rc, out = run("--ddp-port", str(rest.port), "wake", "127.0.0.1", env={"BM_RP_PROFILE": prof2, "GAMELAB_RP_HOST": "no-such-host.invalid"})
    check(rc == 1 and "not paired" in out and not rest.wakes, "wake: an unpaired console is refused, nothing sent: %s" % out.strip().splitlines()[-1])
    rc, out = run("--ddp-port", str(rest.port), "wake", "127.0.0.1", env={"BM_RP_PROFILE": os.path.join(tmp, "missing.json"), "GAMELAB_RP_HOST": "no-such-host.invalid"})
    check(rc == 1 and not rest.wakes, "wake: no profile here and no mac2 to ask: not sent (%s)" % out.strip().splitlines()[-1][:90])

    t0 = time.time()
    rc, out = run("--ddp-port", str(rest.port), "wake", "127.0.0.1", "--wait", "20", env=env)
    check(rc == 0 and "is awake after" in out and rest.state == "awake", "wake: the fake console woke and the tool saw it (%.1f s)" % (time.time() - t0))
    w = rest.wakes[0] if rest.wakes else ""
    check(w == "WAKEUP * HTTP/1.1\nuser-credential:%s\nclient-type:vr\nauth-type:R\nmodel:w\napp-type:r\ndevice-discovery-protocol-version:00030010\n" % CRED,
          "wake: the packet is the Remote Play wake packet with the profile's credential (%d sent)" % len(rest.wakes))
    check(CRED not in out and KEY not in out, "wake: the credential and the key are never printed")
    rc, out = run("--ddp-port", str(rest.port), "wake", "127.0.0.1", env=env)
    check("already awake" in out and len(rest.wakes) == len(rest.wakes), "wake: a second wake finds it awake and sends nothing")
    print("all passed" if not fails else "FAILED: %d" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
