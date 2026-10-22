#!/usr/bin/env python3
import base64
import http.client
import os
import shutil
import socket
import struct
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
PORT = 18085
TINY_PORT = 18086
UPLOADS = os.path.join(ROOT, "tests", "uploads-limits")


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def request(method, path, body=None, headers=None, timeout=5, port=PORT):
    conn = http.client.HTTPConnection(HOST, port, timeout=timeout)
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


def wait_tcp(proc, port):
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError("server exited early")
        try:
            s = socket.create_connection((HOST, port), timeout=0.2)
            s.close()
            return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def raw_exchange(payload, timeout=2):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.sendall(payload)
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    return data


def status_of(data):
    try:
        return int(data.split(b" ", 2)[1])
    except (IndexError, ValueError):
        raise AssertionError("malformed HTTP response: %r" % data[:200])


def expect_raw_status(payload, expected, label):
    data = raw_exchange(payload)
    status = status_of(data)
    check(status == expected,
          "%s: expected %d, got %d: %r" % (label, expected, status, data[:200]))


def abort_request(payload):
    s = socket.create_connection((HOST, PORT), timeout=2)
    s.sendall(payload)
    s.close()


def multipart_body(boundary, parts, final=True):
    b = boundary.encode()
    out = b""
    for headers, data in parts:
        out += b"--" + b + b"\r\n" + headers + b"\r\n\r\n" + data + b"\r\n"
    out += b"--" + b
    if final:
        out += b"--"
    out += b"\r\n"
    return out


def file_part(name, filename, data):
    return (
        b'Content-Disposition: form-data; name="' + name.encode() +
        b'"; filename="' + filename.encode() + b'"\r\n'
        b"Content-Type: application/octet-stream",
        data,
    )


def field_part(name, data):
    return (
        b'Content-Disposition: form-data; name="' + name.encode() + b'"',
        data,
    )


def upload_count():
    return len(os.listdir(UPLOADS))


def ws_connect(path):
    s = socket.create_connection((HOST, PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
        % (path, HOST, PORT, key)
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise AssertionError("websocket handshake closed")
        buf += chunk
    head, _ = buf.split(b"\r\n\r\n", 1)
    check(b" 101 " in head.split(b"\r\n", 1)[0],
          "websocket handshake failed: %r" % head)
    return s


def ws_send_text(s, payload):
    payload = payload.encode()
    mask = os.urandom(4)
    masked = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + masked)


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            return None
        out += chunk
    return out


def ws_recv_close(s):
    h = recv_exact(s, 2)
    if h is None:
        return None
    opcode = h[0] & 0x0f
    n = h[1] & 0x7f
    if n == 126:
        ext = recv_exact(s, 2)
        if ext is None:
            return None
        n = struct.unpack("!H", ext)[0]
    elif n == 127:
        ext = recv_exact(s, 8)
        if ext is None:
            return None
        n = struct.unpack("!Q", ext)[0]
    payload = recv_exact(s, n) if n else b""
    if payload is None:
        return None
    return opcode, payload


def main():
    shutil.rmtree(UPLOADS, ignore_errors=True)
    proc = subprocess.Popen(
        [os.path.join(ROOT, "tmg"),
         os.path.join("tests", "fixtures", "http_limits.lua")],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_ready(proc)

        status, _, body = request("GET", "/health")
        check(status == 200 and body == b"ok", "health failed")

        long_path = "/" + ("a" * 64)
        expect_raw_status(
            b"GET " + long_path.encode() + b" HTTP/1.1\r\n"
            b"Host: localhost\r\nConnection: close\r\n\r\n",
            414, "url length limit")

        headers = b"Host: localhost\r\nConnection: close\r\n"
        for i in range(7):
            headers += b"X-%d: 1\r\n" % i
        expect_raw_status(
            b"GET /health HTTP/1.1\r\n" + headers + b"\r\n",
            431, "header count limit")

        expect_raw_status(
            b"GET /health HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Connection: close\r\n"
            b"X-Long: " + (b"a" * 300) + b"\r\n\r\n",
            431, "header byte limit")

        expect_raw_status(
            b"POST /echo HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Content-Length: 2049\r\n"
            b"Connection: close\r\n\r\n",
            413, "content-length body limit")

        chunked = b"801\r\n" + (b"a" * 2049) + b"\r\n0\r\n\r\n"
        expect_raw_status(
            b"POST /echo HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"Connection: close\r\n\r\n" + chunked,
            413, "chunked body limit")

        expect_raw_status(
            b"POST /echo HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"Connection: close\r\n\r\n"
            b"zz\r\nbad\r\n0\r\n\r\n",
            400, "malformed chunk size")

        expect_raw_status(
            b"POST /echo HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Content-Length: nope\r\n"
            b"Connection: close\r\n\r\n",
            400, "invalid content-length")

        status, _, _ = request("GET", "/big")
        check(status == 500, "oversized response should be 500")

        large_static = os.path.join(ROOT, "tests", "fixtures", "public",
                                    "assets", "boundary-large.txt")
        with open(large_static, "wb") as f:
            f.write(b"x" * 33)
        try:
            status, _, _ = request("GET", "/static/assets/boundary-large.txt")
            check(status == 500, "oversized static file should be 500")
        finally:
            os.unlink(large_static)

        status, headers, body = request("GET", "/static/assets/app.css")
        check(status == 200 and body == b"body { color: red; }\n",
              "normal static file failed")
        check(headers.get("transfer-encoding") == "chunked",
              "normal static file should be chunked")

        status, _, body = request("GET", "/echo-path/%zz")
        check(status == 200 and body == b"%zz", "invalid percent escaped")
        status, _, body = request("GET", "/echo-path/%2")
        check(status == 200 and body == b"%2", "truncated percent escaped")
        status, _, body = request("GET", "/echo-path/%")
        check(status == 200 and body == b"%", "lone percent escaped")

        expect_raw_status(
            b"GET /echo-path/%00 HTTP/1.1\r\n"
            b"Host: localhost\r\nConnection: close\r\n\r\n",
            400, "nul percent decode")

        status, _, _ = request("GET", "/static/%2e%2e/secret.txt")
        check(status == 404, "encoded traversal should be blocked")
        status, _, _ = request("GET", "/static/%252e%252e/secret.txt")
        check(status == 404, "double-encoded traversal should not resolve")
        status, _, _ = request("GET", "/echo-path/a%2fb")
        check(status == 404, "encoded slash should not match a two-part route")

        abort_request(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")
        for _ in range(5):
            abort_request(b"POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                          b"Content-Length: 100\r\n\r\npartial")
        time.sleep(0.1)
        status, _, body = request("GET", "/health")
        check(status == 200 and body == b"ok",
              "server failed after aborted requests")

        boundary = "----limitboundary"
        content_type = "multipart/form-data; boundary=" + boundary

        valid = multipart_body(boundary, [file_part("upload", "a.txt", b"abc")])
        status, _, body = request(
            "POST", "/upload", body=valid,
            headers={"Content-Type": content_type},
        )
        check(status == 200 and body == b"a.txt:3",
              "valid multipart upload failed: " + repr(body))
        baseline = upload_count()
        check(baseline == 1, "valid upload did not create one file")

        status, _, _ = request(
            "POST", "/upload", body=valid,
            headers={"Content-Type": "multipart/form-data"},
        )
        check(status == 400, "multipart without boundary should be 400")

        malformed_header = multipart_body(boundary, [(
            b'Content-Disposition: form-data; name="upload"; '
            b'filename="bad.txt"\r\nBrokenHeader',
            b"x",
        )])
        status, _, _ = request(
            "POST", "/upload", body=malformed_header,
            headers={"Content-Type": content_type},
        )
        check(status == 400, "malformed multipart header should be 400")
        check(upload_count() == baseline,
              "malformed multipart should not leave files")

        missing_disposition = multipart_body(boundary, [(
            b"Content-Type: text/plain",
            b"x",
        )])
        status, _, _ = request(
            "POST", "/upload", body=missing_disposition,
            headers={"Content-Type": content_type},
        )
        check(status == 400, "multipart without field name should be 400")
        check(upload_count() == baseline,
              "multipart without field name should not leave files")

        truncated = multipart_body(
            boundary, [file_part("upload", "partial.txt", b"abc")],
            final=False,
        )
        status, _, _ = request(
            "POST", "/upload", body=truncated,
            headers={"Content-Type": content_type},
        )
        check(status == 400, "truncated multipart should be 400")
        check(upload_count() == baseline,
              "truncated multipart should clean up files")

        too_many_parts = multipart_body(
            boundary, [field_part("n%d" % i, b"x") for i in range(5)])
        status, _, _ = request(
            "POST", "/form", body=too_many_parts,
            headers={"Content-Type": content_type},
        )
        check(status == 400, "multipart part limit should be enforced")

        too_many_files = multipart_body(boundary, [
            file_part("upload", "a.txt", b"a"),
            file_part("upload", "b.txt", b"b"),
            file_part("upload", "c.txt", b"c"),
        ])
        status, _, _ = request(
            "POST", "/upload", body=too_many_files,
            headers={"Content-Type": content_type},
        )
        check(status == 400, "multipart file limit should be enforced")
        check(upload_count() == baseline,
              "multipart file limit should clean up files")

        repeated = multipart_body(boundary, [
            field_part("name", b"first"),
            field_part("name", b"second"),
            field_part("last", b"yes"),
        ])
        status, _, body = request(
            "POST", "/form", body=repeated,
            headers={"Content-Type": content_type},
        )
        check(status == 200 and body == b"second:yes",
              "repeated multipart field should use the last value")

        ws = ws_connect("/ws")
        ws_send_text(ws, "x" * 33)
        try:
            frame = ws_recv_close(ws)
        except socket.timeout:
            frame = None
        ws.close()
        if frame is not None:
            opcode, payload = frame
            check(opcode == 8, "oversized websocket should close")
            if len(payload) >= 2:
                check(struct.unpack("!H", payload[:2])[0] == 1009,
                      "oversized websocket should use close code 1009")

        abort_request(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")
        status, _, body = request("GET", "/health")
        check(status == 200 and body == b"ok",
              "server failed after websocket limit test")

        status, _, body = request("GET", "/shutdown")
        check(status == 200 and body == b"bye", "shutdown failed")
        proc.wait(timeout=5)
        check(proc.returncode == 0, "server exited with %s" % proc.returncode)

        tiny = subprocess.Popen(
            [os.path.join(ROOT, "tmg"),
             os.path.join("tests", "fixtures", "http_tiny_response.lua")],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_tcp(tiny, TINY_PORT)
            status, _, body = request("GET", "/ok", port=TINY_PORT)
            check(status == 500,
                  "tiny max_response should produce 500, got %d" % status)
            check(b"max_response" in body,
                  "tiny max_response error body missing: %r" % body)
            check(tiny.poll() is None,
                  "tiny max_response response crashed the server")
        finally:
            if tiny.poll() is None:
                tiny.kill()
                tiny.wait(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        shutil.rmtree(UPLOADS, ignore_errors=True)

    print("ok: boundary tests passed")


if __name__ == "__main__":
    main()
