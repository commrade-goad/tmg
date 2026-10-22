#!/usr/bin/env python3
import http.cookiejar
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HOST = "127.0.0.1"
PORT = 18087
BASE = "http://%s:%d" % (HOST, PORT)


class Client:
    def __init__(self):
        jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(jar),
        )

    def request(self, method, path, form=None):
        data = None
        headers = {}
        if form is not None:
            data = urllib.parse.urlencode(form).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        req = urllib.request.Request(BASE + path, data=data, headers=headers,
                                     method=method)
        try:
            with self.opener.open(req, timeout=5) as resp:
                return (resp.status,
                        {k.lower(): v for k, v in resp.headers.items()},
                        resp.read())
        except urllib.error.HTTPError as exc:
            return (exc.code,
                    {k.lower(): v for k, v in exc.headers.items()},
                    exc.read())


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def wait_ready(client, proc):
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError("server exited early")
        try:
            status, _, body = client.request("GET", "/health")
            if status == 200 and body == b"ok":
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def main():
    client = Client()
    proc = subprocess.Popen(
        [os.path.join(ROOT, "tmg"),
         os.path.join("tests", "react_counter", "app.lua")],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_ready(client, proc)

        status, _, body = client.request("GET", "/")
        check(status == 200, "counter page failed")
        check(b'id="count">0</span>' in body, "counter did not start at zero")
        check(b'hx-post="/counter/increment"' in body,
              "counter button missing htmx increment")
        check(b"/vendor/htmx.min.js" in body, "counter missing local htmx")
        check(b"/static/app.css" in body, "counter missing stylesheet")

        status, headers, body = client.request("GET", "/vendor/htmx.min.js")
        check(status == 200 and b"var htmx" in body[:200],
              "htmx asset not served")
        check("javascript" in headers.get("content-type", ""),
              "htmx content type")

        status, _, body = client.request("GET", "/static/app.css")
        check(status == 200 and b".increment" in body,
              "counter stylesheet not served")
        check(b"animation" not in body and b"@keyframes" not in body,
              "counter stylesheet should not animate anything")

        status, _, body = client.request("GET", "/counter")
        check(status == 200 and body.strip() == b"0",
              "counter fragment did not start at zero")

        status, _, body = client.request("POST", "/counter/increment")
        check(status == 200 and body.strip() == b"1",
              "first increment failed: %r" % body)

        status, _, body = client.request("POST", "/counter/increment")
        check(status == 200 and body.strip() == b"2",
              "second increment failed: %r" % body)

        status, _, body = client.request("GET", "/")
        check(status == 200 and b'id="count">2</span>' in body,
              "counter did not persist across requests")

        other = Client()
        status, _, body = other.request("GET", "/")
        check(status == 200 and b'id="count">0</span>' in body,
              "counter sessions are not isolated")

        status, _, body = other.request("POST", "/counter/increment")
        check(status == 200 and body.strip() == b"1",
              "other session increment failed")

        status, _, body = client.request("POST", "/counter/increment")
        check(status == 200 and body.strip() == b"3",
              "original session counter was lost")

        status, _, body = other.request("GET", "/counter")
        check(status == 200 and body.strip() == b"1",
              "other session changed unexpectedly")

        print("ok: react counter tests passed")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        err = proc.stderr.read()
        if err and "http:" in err:
            print("--- server stderr ---", file=sys.stderr)
            print(err, file=sys.stderr)


if __name__ == "__main__":
    main()
