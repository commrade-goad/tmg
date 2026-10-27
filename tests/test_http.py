#!/usr/bin/env python3
import base64
import hashlib
import http.client
import os
import socket
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 18080
HOST = "127.0.0.1"


def request(method, path, body=None, headers=None, port=PORT):
    conn = http.client.HTTPConnection(HOST, port, timeout=5)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, hdrs, data


def wait_ready(proc, port=PORT):
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError("server exited early")
        try:
            status, _, _ = request("GET", "/health", port=port)
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def raw_exchange(payload, port=PORT):
    s = socket.create_connection((HOST, port), timeout=5)
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


class WSClient:
    def __init__(self, path):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.buf = b""
        self.key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
            % (path, HOST, PORT, self.key)
        )
        self.sock.sendall(req.encode())
        head = self._recv_until(b"\r\n\r\n")
        header = head[:-4]
        self.status = int(header.split(b"\r\n", 1)[0].split()[1])
        self.headers = {}
        for line in header.split(b"\r\n")[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                self.headers[k.strip().lower()] = v.strip()

    def _recv_until(self, marker):
        while marker not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("websocket closed during handshake")
            self.buf += chunk
        idx = self.buf.index(marker) + len(marker)
        out, self.buf = self.buf[:idx], self.buf[idx:]
        return out

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("websocket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, payload, opcode=1, fin=True):
        if isinstance(payload, str):
            payload = payload.encode()
        n = len(payload)
        b0 = (0x80 if fin else 0) | opcode
        if n < 126:
            header = bytes([b0, 0x80 | n])
        elif n < 65536:
            header = bytes([b0, 0x80 | 126]) + struct.pack("!H", n)
        else:
            header = bytes([b0, 0x80 | 127]) + struct.pack("!Q", n)
        mask = os.urandom(4)
        masked = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def recv(self):
        h = self._recv_exact(2)
        fin = bool(h[0] & 0x80)
        opcode = h[0] & 0x0F
        masked = bool(h[1] & 0x80)
        n = h[1] & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._recv_exact(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._recv_exact(8))[0]
        check(not masked, "server frames must not be masked")
        payload = self._recv_exact(n)
        return opcode, payload, fin

    def close(self):
        self.sock.close()


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

        status, _, body = request(
            "GET", "/echo-cookie",
            headers={"Cookie": "session_id=first; session_id=second"},
        )
        check(status == 200 and body == b"second",
              "duplicate cookie should be deterministic")

        status, _, body = request("GET", "/cookie-invalid")
        check(status == 200 and b"secure must be a boolean" in body,
              "invalid cookie option should be rejected: " + repr(body))

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

        uploads_dir = os.path.join(ROOT, "tests", "uploads")
        upload_count = len(os.listdir(uploads_dir))
        truncated = (
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="upload"; filename="partial.txt"\r\n'
            b"Content-Type: text/plain\r\n\r\n" + file_data + b"\r\n"
            b"--" + boundary.encode() + b"\r\n"
        )
        status, _, _ = request(
            "POST", "/upload", body=truncated,
            headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
        )
        check(status == 400, "truncated multipart should be 400")
        check(len(os.listdir(uploads_dir)) == upload_count,
              "truncated multipart should clean up uploaded files")

        repeated = (
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="name"\r\n\r\nfirst\r\n'
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="name"\r\n\r\nsecond\r\n'
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="age"\r\n\r\n36\r\n'
            b"--" + boundary.encode() + b"--\r\n"
        )
        status, _, body = request(
            "POST", "/form", body=repeated,
            headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
        )
        check(status == 200 and body == b"second:36",
              "repeated multipart field should use the last value")

        too_many = b"".join(
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="n"\r\n\r\nx\r\n'
            for _ in range(65)
        ) + b"--" + boundary.encode() + b"--\r\n"
        status, _, _ = request(
            "POST", "/form", body=too_many,
            headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
        )
        check(status == 400, "multipart part limit should be enforced")

        status, _, body = request("GET", "/render")
        check(status == 200 and body == b"\nHello TMG!\n", "template render failed: " + repr(body))

        cache_tpl = os.path.join(ROOT, "tests", "fixtures", "templates",
                                 "cache_test.tmg")
        with open(cache_tpl, "w") as f:
            f.write("% local data = ... %v1:% tmg.print(data.name) %")
        try:
            request("GET", "/cache-clear")

            def cache_stats():
                _, _, data = request("GET", "/cache-stats")
                fields = data.decode().split(",")
                return fields[0], [int(v) for v in fields[1:]]

            mode, s0 = cache_stats()
            check(mode == "development" and s0[0] == 0,
                  "cache clear failed: %r" % ((mode, s0),))

            status, _, body = request("GET", "/render-cache")
            check(status == 200 and body == b"v1:cache",
                  "cached template first render failed: " + repr(body))
            _, s1 = cache_stats()
            check(s1[0] == 1 and s1[2] == s0[2] + 1,
                  "cache miss not recorded: %r" % (s1,))

            status, _, body = request("GET", "/render-cache")
            check(status == 200 and body == b"v1:cache",
                  "cached template second render failed")
            _, s2 = cache_stats()
            check(s2[0] == 1 and s2[1] == s1[1] + 1,
                  "cache hit not recorded: %r" % (s2,))

            with open(cache_tpl, "w") as f:
                f.write("% local data = ... %v2:% tmg.print(data.name) %")
            stamp = time.time_ns()
            os.utime(cache_tpl, ns=(stamp + 1000000000, stamp + 1000000000))

            status, _, body = request("GET", "/render-cache")
            check(status == 200 and body == b"v2:cache",
                  "template reload failed: " + repr(body))
            _, s3 = cache_stats()
            check(s3[0] == 1 and s3[3] == s2[3] + 1,
                  "cache reload not recorded: %r" % (s3,))

            os.unlink(cache_tpl)
            status, _, _ = request("GET", "/render-cache")
            check(status == 500, "deleted template should fail")
            _, s4 = cache_stats()
            check(s4[0] == 0, "deleted template should be evicted")
        finally:
            if os.path.exists(cache_tpl):
                os.unlink(cache_tpl)

        status, headers, body = request("GET", "/escape")
        expected = b"&lt;b&gt;&quot;&amp;&#39;&lt;/b&gt;|<i>raw</i>\n"
        check(status == 200 and body == expected,
              "html autoescape failed: " + repr(body))
        check("text/html" in headers.get("content-type", ""),
              "html autoescape content type")

        status, _, body = request("GET", "/escape-off")
        check(status == 200 and body == b"<b>\"&'</b>|<i>raw</i>\n",
              "autoescape opt-out failed: " + repr(body))

        status, _, body = request("GET", "/stash-response")
        check(status == 200 and body == b"stored", "stash response failed")
        status, _, body = request("GET", "/use-stale-response")
        check(status == 200 and b"response is detached" in body,
              "stale response should fail deterministically: " + repr(body))

        status, _, _ = request("GET", "/too-large-response")
        check(status == 500, "oversized response should fail with 500")

        status, _, _ = request("GET", "/render-too-large")
        check(status == 500, "oversized render should fail with 500")

        # Regression: a multi-write (chunked static) response on a reused
        # keep-alive connection must not stall ~40 ms on Nagle + delayed ACK.
        ka = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        try:
            worst = 0.0
            for _ in range(6):
                t0 = time.perf_counter()
                ka.sendall(b"GET /static/assets/app.css HTTP/1.1\r\nHost: x\r\n\r\n")
                buf = b""
                while not buf.endswith(b"0\r\n\r\n"):
                    chunk = ka.recv(65536)
                    check(chunk, "keep-alive static: connection closed early")
                    buf += chunk
                worst = max(worst, time.perf_counter() - t0)
            check(worst < 0.030,
                  "keep-alive static latency %.1f ms (TCP_NODELAY missing?)" % (worst * 1000))
        finally:
            ka.close()

        status, headers, body = request("GET", "/static/assets/app.css")
        check(status == 200 and body == b"body { color: red; }\n",
              "static css failed: " + repr(body))
        check("text/css" in headers.get("content-type", ""),
              "static css content type")
        check(headers.get("x-content-type-options") == "nosniff",
              "static nosniff header")
        check(headers.get("transfer-encoding") == "chunked",
              "static GET should use chunked encoding")
        check("content-length" not in headers,
              "static GET should not send content-length")

        large = b"0123456789abcdef" * 10000
        large_path = os.path.join(ROOT, "tests", "fixtures", "public",
                                  "assets", "large.bin")
        with open(large_path, "wb") as f:
            f.write(large)
        try:
            status, headers, body = request("GET", "/static/assets/large.bin")
            check(status == 200 and body == large,
                  "static large transfer failed")
            check(headers.get("transfer-encoding") == "chunked",
                  "static large transfer should be chunked")
        finally:
            os.unlink(large_path)

        status, headers, body = request("HEAD", "/static/assets/app.css")
        check(status == 200 and body == b"",
              "static HEAD failed")
        check(headers.get("content-length") == str(len(b"body { color: red; }\n")),
              "static HEAD content-length")
        check("transfer-encoding" not in headers,
              "static HEAD should not be chunked")

        conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
        conn.request("GET", "/static/assets/app.css")
        r1 = conn.getresponse()
        check(r1.status == 200 and r1.read() == b"body { color: red; }\n",
              "static keepalive first request failed")
        conn.request("GET", "/health")
        r2 = conn.getresponse()
        check(r2.status == 200 and r2.read() == b"ok",
              "static keepalive follow-up failed")
        conn.close()

        status, _, body = request("GET", "/static/assets/")
        check(status == 200 and body == b"<h1>assets</h1>\n",
              "static directory index failed: " + repr(body))

        status, _, _ = request("GET", "/static/%2e%2e/secret.txt")
        check(status == 404, "static traversal should be blocked")

        status, headers, _ = request("POST", "/static/assets/app.css")
        check(status == 405 and "GET" in headers.get("allow", ""),
              "static method should be 405")

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

        start = time.time()
        idle = raw_exchange(
            b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        )
        elapsed = time.time() - start
        check(b" 200 " in idle.split(b"\r\n", 1)[0], "idle keep-alive request failed")
        check(b"Connection: keep-alive" in idle, "keep-alive header missing")
        check(elapsed >= 0.8 and elapsed < 5, "idle connection was not reaped")

        hello = WSClient("/ws/hello")
        check(hello.status == 101, "websocket hello handshake status")
        opcode, payload, fin = hello.recv()
        check(opcode == 1 and fin and payload == b"welcome",
              "websocket initial send failed: %r" % payload)
        WSClient.close(hello)

        ws = WSClient("/ws/room42")
        check(ws.status == 101, "websocket handshake status")
        expected_accept = base64.b64encode(
            hashlib.sha1((ws.key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        )
        check(ws.headers.get(b"sec-websocket-accept") == expected_accept,
              "websocket accept key mismatch")
        ws.send("hello")
        opcode, payload, fin = ws.recv()
        check(opcode == 1 and fin and payload == b"room42:txt:hello",
              "websocket text echo failed: %r" % payload)
        ws.send("frag", opcode=1, fin=False)
        ws.send("mented", opcode=0, fin=True)
        opcode, payload, fin = ws.recv()
        check(opcode == 1 and fin and payload == b"room42:txt:fragmented",
              "websocket fragmented message failed: %r" % payload)
        ws.send(b"\x00\x01\xff", opcode=2)
        opcode, payload, fin = ws.recv()
        check(opcode == 2 and fin and payload == b"room42:bin:\x00\x01\xff",
              "websocket binary echo failed: %r" % payload)
        ws.send(b"ping", opcode=9)
        opcode, payload, fin = ws.recv()
        check(opcode == 10 and payload == b"ping", "websocket pong failed")
        ws.send(struct.pack("!H", 1000) + b"bye", opcode=8)
        opcode, payload, fin = ws.recv()
        check(opcode == 8 and payload[:2] == struct.pack("!H", 1000),
              "websocket close reply failed: %r" % payload)
        check(ws.sock.recv(1) == b"", "websocket connection not closed")
        WSClient.close(ws)

        closer = WSClient("/ws/close-on-message")
        closer.send("bye")
        opcode, payload, fin = closer.recv()
        check(opcode == 8 and payload[:2] == struct.pack("!H", 1000) and
              payload[2:] == b"done",
              "websocket server close failed: %r" % payload)
        check(closer.sock.recv(1) == b"", "server-close connection stayed open")
        WSClient.close(closer)

        def ws_reject(version=None, key=True):
            s = socket.create_connection((HOST, PORT), timeout=5)
            headers = (
                "GET /ws/room42 HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
            )
            if key:
                headers += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            headers += "Sec-WebSocket-Version: %s\r\n\r\n" % (version or 13)
            s.sendall(headers.encode())
            data = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            s.close()
            return data

        bad_version = ws_reject(version=12)
        check(b" 426 " in bad_version.split(b"\r\n", 1)[0],
              "websocket bad version should be 426")
        check(b"Sec-WebSocket-Version: 13" in bad_version,
              "websocket version header missing")
        no_key = ws_reject(key=False)
        check(b" 400 " in no_key.split(b"\r\n", 1)[0],
              "websocket missing key should be 400")

        status, _, body = request("GET", "/shutdown")
        check(status == 200 and body == b"bye", "shutdown response failed")
        proc.wait(timeout=5)
        check(proc.returncode == 0, "server exited with %s" % proc.returncode)

        pending = subprocess.Popen(
            [os.path.join(ROOT, "tmg"),
             os.path.join("tests", "fixtures", "http_pending_shutdown.lua")],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_ready(pending, 18084)

            disconnect = socket.create_connection((HOST, 18084), timeout=5)
            disconnect.sendall(
                b"GET /big HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Connection: close\r\n\r\n"
            )
            check(len(disconnect.recv(1024)) > 0, "big response did not start")
            disconnect.close()
            time.sleep(0.1)
            status, _, body = request("GET", "/health", port=18084)
            check(status == 200 and body == b"ok",
                  "server failed after client disconnect during write")

            response = raw_exchange(
                b"GET /shutdown-big HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Connection: close\r\n\r\n",
                18084,
            )
            check(b" 200 " in response.split(b"\r\n", 1)[0],
                  "pending shutdown response failed")
            check(b"Content-Length: 4194304" in response,
                  "pending shutdown content length missing")
            check(response.endswith(b"z" * 1024), "pending shutdown body truncated")
            pending.wait(timeout=10)
            check(pending.returncode == 0,
                  "pending shutdown exit code %s" % pending.returncode)
        finally:
            if pending.poll() is None:
                pending.kill()
                pending.wait()
            err = pending.stderr.read()
            check("ERR:" not in err, "pending shutdown stderr: " + err)

        for case, expected in [
            ("negative", "max_body must be >= 0"),
            ("timeout_overflow", "header_timeout is too large"),
            ("size_type", "max_body must be an integer"),
            ("fraction", "max_body must be an integer"),
            ("bad_port", "invalid port"),
        ]:
            env = dict(os.environ)
            env["TMG_BAD_CONFIG"] = case
            bad = subprocess.run(
                [os.path.join(ROOT, "tmg"),
                 os.path.join("tests", "fixtures", "http_bad_config.lua")],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
                env=env,
            )
            check(bad.returncode != 0, "invalid config should fail: " + case)
            check(expected in bad.stderr,
                  "invalid config message missing for %s: %s" % (case, bad.stderr))

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
