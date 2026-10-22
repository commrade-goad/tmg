#!/usr/bin/env python3
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HOST = "127.0.0.1"
PORT = 18088
BASE = "http://%s:%d" % (HOST, PORT)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def request(path):
    req = urllib.request.Request(BASE + path, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def wait_ready(proc):
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError("server exited early")
        try:
            status, body = request("/health")
            if status == 200 and body == b"ok":
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def run_mode(mode):
    env = os.environ.copy()
    env["TMG_TEST_CACHE"] = mode
    proc = subprocess.Popen(
        [os.path.join(ROOT, "tmg"),
         os.path.join("tests", "fixtures", "http_delim.lua")],
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_ready(proc)

        status, body = request("/hash")
        check(status == 200, "%s /hash failed with %d" % (mode, status))
        check(b"Hash: world" in body,
              "%s hash template did not render: %r" % (mode, body[:200]))
        check(b"percent: 100%" in body,
              "%s percent stayed literal" % mode)
        check(b"at: @@ literal" in body,
              "%s at sign stayed literal" % mode)
        check(b"# tmg.print" not in body,
              "%s hash code block was not parsed" % mode)

        status, body = request("/at")
        check(status == 200, "%s /at failed with %d" % (mode, status))
        check(b"At: world" in body,
              "%s at template did not render: %r" % (mode, body[:200]))
        check(b"percent: 100%" in body,
              "%s percent stayed literal in at template" % mode)
        check(b"hash: # literal" in body,
              "%s hash stayed literal in at template" % mode)
        check(b"@@ tmg.print" not in body,
              "%s at code block was not parsed" % mode)

        status, body = request("/hash")
        check(status == 200 and b"Hash: world" in body,
              "%s delimiter switch back failed" % mode)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        err = proc.stderr.read()
        if err and "http:" in err:
            print("--- server stderr (%s) ---" % mode, file=sys.stderr)
            print(err, file=sys.stderr)


def main():
    for mode in ("off", "development", "production"):
        run_mode(mode)
    print("ok: template separator HTTP tests passed")


if __name__ == "__main__":
    main()
