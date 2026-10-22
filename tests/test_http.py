#!/usr/bin/env python3
import http.client
import os
import socket
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 18080
HOST = "127.0.0.1"


def request(method, path, body=None, headers=None):
    conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, hdrs, data


def wait_ready(proc):
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError("server exited early")
        try:
            status, _, _ = request("GET", "/health")
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def raw_exchange(payload):
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(payload)
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    return data


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    shutil.rmtree(os.path.join(ROOT, "tests", "uploads"), ignore_errors=True)
    proc = subprocess.Popen(
        [os.path.join(ROOT, "tmg"), os.path.join("tests", "fixtures", "http_app.lua")],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_ready(proc)

        status, _, body = request("GET", "/")
        check(status == 200 and body == b"hello", "basic GET failed")

        status, _, body = request("GET", "/tu/requests/42")
        check(status == 200 and body == b"id=42", "dynamic GET param failed")

        status, _, body = request("POST", "/tu/requests/42/approve", body=b"")
        check(status == 200 and body == b"approved 42", "dynamic POST param failed")

        status, _, body = request("GET", "/tu/requests/7/pdf")
        check(status == 200 and body == b"pdf 7", "dynamic pdf route failed")

        status, headers, _ = request("GET", "/set-cookie")
        cookie = headers.get("set-cookie", "")
        check(status == 200, "cookie status")
        check("session_id=abc123" in cookie, "cookie value missing")
        check("HttpOnly" in cookie, "HttpOnly missing")
        check("SameSite=Lax" in cookie, "SameSite missing")
        check("Max-Age=3600" in cookie, "Max-Age missing")

        status, _, body = request("GET", "/echo-cookie", headers={"Cookie": "session_id=abc123"})
        check(status == 200 and body == b"abc123", "cookie parsing failed")

        form = b"name=Ada&age=36"
        status, _, body = request(
            "POST", "/form", body=form,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        check(status == 200 and body == b"Ada:36", "urlencoded form parsing failed")

        boundary = "----tmgboundary42"
        file_data = b"hello upload\n"
        multipart = (
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="upload"; filename="scan.txt"\r\n'
            b"Content-Type: text/plain\r\n\r\n" + file_data + b"\r\n"
            b"--" + boundary.encode() + b"--\r\n"
        )
        status, _, body = request(
            "POST", "/upload", body=multipart,
            headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
        )
        check(status == 200, "multipart status")
        parts = body.decode().split("|")
        check(len(parts) == 5, "multipart response malformed: " + body.decode())
        check(parts[0] == "upload", "multipart field")
        check(parts[1] == "scan.txt", "multipart filename")
        check(parts[2] == "text/plain", "multipart content type")
        check(parts[3] == str(len(file_data)), "multipart size")
        path = os.path.join(ROOT, parts[4])
        check(os.path.isfile(path), "uploaded file missing: " + path)
        with open(path, "rb") as f:
            check(f.read() == file_data, "uploaded file contents mismatch")

        status, _, body = request("GET", "/render")
        check(status == 200 and body == b"Hello TMG!\n", "template render failed: " + repr(body))

        status, _, _ = request("GET", "/boom")
        check(status == 500, "handler error should be 500")
        status, _, body = request("GET", "/health")
        check(status == 200 and body == b"ok", "server loop died after handler error")

        status, _, _ = request("GET", "/missing")
        check(status == 404, "404 expected")
        status, headers, _ = request("GET", "/form")
        check(status == 405, "405 expected")
        check("POST" in headers.get("allow", ""), "Allow header missing")

        chunked = (
            b"POST /form HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Content-Type: application/x-www-form-urlencoded\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"Connection: close\r\n\r\n"
            b"4\r\nname\r\n1\r\n=\r\n3\r\nAda\r\n1\r\n&\r\n3\r\nage\r\n1\r\n=\r\n2\r\n36\r\n0\r\n\r\n"
        )
        resp = raw_exchange(chunked)
        check(b" 200 " in resp.split(b"\r\n", 1)[0], "chunked request status")
        check(resp.endswith(b"Ada:36"), "chunked request body")

        body = b"name=A&age=1"
        expect_req = (
            b"POST /form HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Content-Type: application/x-www-form-urlencoded\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n"
            b"Expect: 100-continue\r\n"
            b"Connection: close\r\n\r\n"
        )
        s = socket.create_connection((HOST, PORT), timeout=5)
        s.sendall(expect_req)
        interim = s.recv(4096)
        check(b" 100 " in interim.split(b"\r\n", 1)[0], "100-continue not sent")
        s.sendall(body)
        final = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            final += chunk
        s.close()
        check(b" 200 " in final.split(b"\r\n", 1)[0], "expect request status")
        check(final.endswith(b"A:1"), "expect request body")

        bad = raw_exchange(b"GET / HTTP/1.1\r\nHost: localhost\r\nBroken Header\r\n\r\n")
        check(b" 400 " in bad.split(b"\r\n", 1)[0], "malformed request should be 400")

        bad_expect = raw_exchange(
            b"POST /form HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Content-Length: 8\r\n"
            b"Expect: unsupported\r\n"
            b"Connection: close\r\n\r\n"
        )
        check(b" 417 " in bad_expect.split(b"\r\n", 1)[0],
              "unsupported Expect should be 417")

        conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
        conn.request("GET", "/health")
        r1 = conn.getresponse()
        b1 = r1.read()
        conn.request("GET", "/health")
        r2 = conn.getresponse()
        b2 = r2.read()
        conn.close()
        check(r1.status == 200 and r2.status == 200 and b1 == b2 == b"ok",
              "keep-alive reuse failed")

        status, _, body = request("GET", "/shutdown")
        check(status == 200 and body == b"bye", "shutdown response failed")
        proc.wait(timeout=5)
        check(proc.returncode == 0, "server exited with %s" % proc.returncode)

        print("ok: all HTTP tests passed")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        err = proc.stderr.read()
        if err and "http:" in err:
            print("--- server stderr ---", file=sys.stderr)
            print(err, file=sys.stderr)
        shutil.rmtree(os.path.join(ROOT, "tests", "uploads"), ignore_errors=True)


if __name__ == "__main__":
    main()
