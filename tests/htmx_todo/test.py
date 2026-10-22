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
PORT = 18081
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
        [os.path.join(ROOT, "tmg"), os.path.join("tests", "htmx_todo", "app.lua")],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_ready(client, proc)

        status, _, body = client.request("GET", "/")
        check(status == 200 and b'id="todo-list"' in body,
              "SPA shell missing todo list")
        check(b'hx-get="/todos"' in body, "SPA shell missing htmx load trigger")
        check(b"/static/htmx.min.js" in body, "SPA shell missing local htmx")
        check(b"/static/app.css" in body, "SPA shell missing stylesheet")

        status, headers, body = client.request("GET", "/static/htmx.min.js")
        check(status == 200 and b"var htmx" in body[:200],
              "htmx asset not served")
        check("javascript" in headers.get("content-type", ""),
              "htmx content type")

        status, _, body = client.request("GET", "/static/app.css")
        check(status == 200 and b".todo-list" in body, "css asset not served")

        status, _, body = client.request("GET", "/todos")
        check(status == 200 and b"No todos yet" in body, "new session not empty")

        status, _, body = client.request("POST", "/todos", {"title": "Buy milk"})
        check(status == 200 and b"Buy milk" in body, "create failed")
        check(b'id="todo-1"' in body, "create missing todo id")

        status, _, body = client.request(
            "POST", "/todos", {"title": "<script>alert(1)</script>"}
        )
        check(status == 200, "second create failed")
        check(b"&lt;script&gt;alert(1)&lt;/script&gt;" in body,
              "todo title was not HTML-escaped")
        check(b"<script>alert(1)</script>" not in body,
              "todo title was emitted raw")

        status, _, body = client.request("GET", "/todos/1/edit")
        check(status == 200, "edit form failed")
        check(b'hx-put="/todos/1"' in body, "edit form missing update route")
        check(b'value="Buy milk"' in body, "edit form missing title")

        status, _, body = client.request("PUT", "/todos/1",
                                         {"title": "Buy oat milk"})
        check(status == 200 and b"Buy oat milk" in body, "update failed")
        check(b"Buy milk" not in body, "old title still visible")

        status, _, body = client.request("POST", "/todos/1/toggle")
        check(status == 200 and b'class="todo done"' in body,
              "toggle did not mark todo done")
        check(b"Undo" in body, "toggle did not switch action label")

        status, _, body = client.request("DELETE", "/todos/2")
        check(status == 200, "delete failed")
        check(b"&lt;script&gt;" not in body, "deleted todo still visible")
        check(b"Buy oat milk" in body, "remaining todo disappeared")

        status, _, body = client.request("POST", "/todos", {"title": "   "})
        check(status == 400, "empty title should be rejected")

        status, _, _ = client.request("GET", "/todos/999/edit")
        check(status == 404, "missing todo should be 404")

        other = Client()
        status, _, body = other.request("GET", "/todos")
        check(status == 200 and b"No todos yet" in body,
              "sessions are not isolated")

        status, _, body = client.request("GET", "/todos")
        check(status == 200 and b"Buy oat milk" in body,
              "original session was lost")

        print("ok: htmx todo tests passed")
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
