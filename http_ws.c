#include "http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef WSLAY_VERSION
#define WSLAY_VERSION "1.1.1"
#endif
#include <wslay/wslay.h>

#define HTTP_WS_MT "tmg.http.websocket"
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

struct http_ws {
    http_conn_t *conn;
    lua_State *L;
    wslay_event_context_ptr ctx;
    int self_ref;
    int on_message_ref;
    int on_close_ref;
    str_t in;
    size_t in_pos;
    str_t out;
    bool writing;
    bool closing;
    bool close_after_write;
    bool in_open_handler;
    bool in_recv;
    bool close_notified;
    bool cleanup_pending;
    bool refs_released;
    char *write_buf;
    uv_write_t write_req;
};

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} sha1_ctx;

static void ws_flush(http_ws_t *ws);
static void ws_notify_close(http_ws_t *ws, uint16_t code, const char *reason,
                            size_t rlen);
static void ws_release_refs(http_ws_t *ws);
static void ws_cleanup_now(http_ws_t *ws);

static uint32_t rol32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) |
               (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_ctx *c) {
    memset(c, 0, sizeof(*c));
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
}

static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t len) {
    c->len += len;
    while (len > 0) {
        size_t n = 64 - c->used;
        if (n > len) n = len;
        memcpy(c->buf + c->used, p, n);
        c->used += n;
        p += n;
        len -= n;
        if (c->used == 64) {
            sha1_block(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad[64];
    size_t padlen = 0;
    pad[padlen++] = 0x80;
    while ((c->used + padlen) % 64 != 56)
        pad[padlen++] = 0;
    sha1_update(c, pad, padlen);

    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenbuf, sizeof(lenbuf));

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void b64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) |
                     (uint32_t)in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tab[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int b64_decode(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s);
    if (n == 0 || n % 4 != 0) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = b64_value((unsigned char)s[i]);
        int b = b64_value((unsigned char)s[i + 1]);
        if (a < 0 || b < 0) return -1;

        bool p2 = s[i + 2] == '=';
        bool p3 = s[i + 3] == '=';
        if (p2 && !p3) return -1;
        int c = p2 ? 0 : b64_value((unsigned char)s[i + 2]);
        int d = p3 ? 0 : b64_value((unsigned char)s[i + 3]);
        if (c < 0 || d < 0) return -1;

        if (o >= cap) return -1;
        out[o++] = (uint8_t)((a << 2) | (b >> 4));
        if (!p2) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(((b & 0x0f) << 4) | (c >> 2));
        }
        if (!p3) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(((c & 0x03) << 6) | d);
        }
        if ((p2 || p3) && i + 4 != n) return -1;
    }
    return (int)o;
}

static bool trim_copy(const char *in, char *out, size_t cap) {
    if (!in) return false;
    while (*in == ' ' || *in == '\t') in++;
    size_t n = strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
    if (n == 0 || n + 1 > cap) return false;
    memcpy(out, in, n);
    out[n] = '\0';
    return true;
}

static bool header_has_token(const char *value, const char *token) {
    if (!value || !token) return false;
    size_t tlen = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if ((size_t)(e - s) == tlen && strncasecmp(s, token, tlen) == 0)
            return true;
    }
    return false;
}

static bool headers_have_token(const http_header_t *headers, size_t n,
                               const char *name, const char *token) {
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(headers[i].name, name) == 0 &&
            header_has_token(headers[i].value, token))
            return true;
    }
    return false;
}

static void ws_reject(http_conn_t *c, int status, const char *msg) {
    if (!c || c->closing) return;
    http_response_reset(c);
    c->status = status;
    c->keep_alive = false;
    c->close_after_write = true;
    http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                     "Content-Type", "text/plain; charset=utf-8", true);
    if (status == 426)
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Sec-WebSocket-Version", "13", true);
    str_clear(&c->res_body);
    str_push(&c->res_body, msg && *msg ? msg : http_status_reason(status));
    c->ended = true;
    http_conn_send_response(c);
}

static void ws_free_resources(http_ws_t *ws) {
    if (!ws) return;
    if (ws->ctx) {
        wslay_event_context_free(ws->ctx);
        ws->ctx = NULL;
    }
    str_deinit(&ws->in);
    str_deinit(&ws->out);
    free(ws->write_buf);
    ws->write_buf = NULL;
}

static void ws_release_refs(http_ws_t *ws) {
    if (!ws || ws->refs_released) return;
    ws->refs_released = true;
    lua_State *L = ws->L;
    ws->L = NULL;
    if (!L) return;
    if (ws->on_message_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ws->on_message_ref);
        ws->on_message_ref = LUA_NOREF;
    }
    if (ws->on_close_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ws->on_close_ref);
        ws->on_close_ref = LUA_NOREF;
    }
    if (ws->self_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ws->self_ref);
        ws->self_ref = LUA_NOREF;
    }
}

static void ws_cleanup_now(http_ws_t *ws) {
    if (!ws) return;
    ws->cleanup_pending = false;
    ws_free_resources(ws);
    ws_release_refs(ws);
}

static void ws_compact_in(http_ws_t *ws) {
    if (!ws || ws->in_pos == 0) return;
    if (ws->in_pos >= ws->in.len) {
        str_clear(&ws->in);
        ws->in_pos = 0;
        return;
    }
    size_t n = ws->in.len - ws->in_pos;
    memmove(ws->in.data, ws->in.data + ws->in_pos, n);
    ws->in.len = n;
    ws->in.data[n] = '\0';
    ws->in_pos = 0;
}

static ssize_t ws_recv_cb(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
                          int flags, void *user_data) {
    (void)flags;
    http_ws_t *ws = (http_ws_t *)user_data;
    if (!ws || ws->in_pos >= ws->in.len) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    size_t n = ws->in.len - ws->in_pos;
    if (n > len) n = len;
    memcpy(buf, ws->in.data + ws->in_pos, n);
    ws->in_pos += n;
    return (ssize_t)n;
}

static ssize_t ws_send_cb(wslay_event_context_ptr ctx, const uint8_t *data,
                          size_t len, int flags, void *user_data) {
    (void)ctx;
    (void)flags;
    http_ws_t *ws = (http_ws_t *)user_data;
    if (!ws) return -1;
    str_push_len(&ws->out, (const char *)data, len);
    return (ssize_t)len;
}

static void ws_write_cb(uv_write_t *req, int status) {
    http_ws_t *ws = (http_ws_t *)req->data;
    if (!ws) return;
    http_conn_t *c = ws->conn;

    ws->writing = false;
    free(ws->write_buf);
    ws->write_buf = NULL;

    if (ws->cleanup_pending) {
        ws_cleanup_now(ws);
        return;
    }

    if (!c) return;
    if (status < 0) {
        http_conn_close(c);
        return;
    }
    if (ws->close_after_write && ws->out.len == 0) {
        http_conn_close(c);
        return;
    }
    ws_flush(ws);
}

static void ws_flush(http_ws_t *ws) {
    if (!ws || ws->writing || ws->in_open_handler || ws->in_recv) return;
    http_conn_t *c = ws->conn;
    if (!c || c->closing) return;

    if (ws->out.len == 0) {
        if (ws->close_after_write)
            http_conn_close(c);
        return;
    }

    char *buf = ws->out.data;
    size_t len = ws->out.len;
    ws->out = str_init(256);
    ws->write_buf = buf;
    ws->writing = true;
    ws->write_req.data = ws;

    uv_buf_t b = uv_buf_init(buf, (unsigned int)len);
    int rc = uv_write(&ws->write_req, (uv_stream_t *)&c->handle, &b, 1,
                      ws_write_cb);
    if (rc != 0) {
        free(ws->write_buf);
        ws->write_buf = NULL;
        ws->writing = false;
        http_conn_close(c);
    }
}

static int ws_msghandler(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") &&
            lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)",
                              luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static void ws_notify_message(http_ws_t *ws, const uint8_t *data, size_t len,
                              bool binary) {
    if (!ws || !ws->L || ws->on_message_ref == LUA_NOREF || ws->closing)
        return;
    lua_State *L = ws->L;
    int top = lua_gettop(L);
    lua_pushcfunction(L, ws_msghandler);
    int errfunc = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ws->on_message_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ws->self_ref);
    lua_pushlstring(L, data ? (const char *)data : "", len);
    lua_pushboolean(L, binary);
    if (lua_pcall(L, 3, 0, errfunc) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        http_log("websocket message handler error: %s",
                 msg ? msg : "(non-string error)");
    }
    lua_settop(L, top);
}

static void ws_notify_close(http_ws_t *ws, uint16_t code, const char *reason,
                            size_t rlen) {
    if (!ws || ws->close_notified) return;
    ws->close_notified = true;
    if (!ws->L || ws->on_close_ref == LUA_NOREF) return;

    lua_State *L = ws->L;
    int top = lua_gettop(L);
    lua_pushcfunction(L, ws_msghandler);
    int errfunc = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ws->on_close_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ws->self_ref);
    lua_pushinteger(L, code);
    lua_pushlstring(L, reason ? reason : "", rlen);
    if (lua_pcall(L, 3, 0, errfunc) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        http_log("websocket close handler error: %s",
                 msg ? msg : "(non-string error)");
    }
    lua_settop(L, top);
}

static void ws_on_msg_recv(wslay_event_context_ptr ctx,
                           const struct wslay_event_on_msg_recv_arg *arg,
                           void *user_data) {
    (void)ctx;
    http_ws_t *ws = (http_ws_t *)user_data;
    if (!ws) return;

    if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
        const char *reason = "";
        size_t rlen = 0;
        if (arg->msg && arg->msg_length >= 2) {
            reason = (const char *)arg->msg + 2;
            rlen = arg->msg_length - 2;
        }
        ws->closing = true;
        ws->close_after_write = true;
        ws_notify_close(ws, arg->status_code ? arg->status_code : 1005,
                        reason, rlen);
    } else if (arg->opcode == WSLAY_TEXT_FRAME ||
               arg->opcode == WSLAY_BINARY_FRAME) {
        ws_notify_message(ws, arg->msg, arg->msg_length,
                          arg->opcode == WSLAY_BINARY_FRAME);
    }
}

void http_ws_feed(http_conn_t *c, const char *data, size_t len) {
    if (!c || !c->ws || len == 0) return;
    http_ws_t *ws = c->ws;
    if (ws->closing) {
        if (ws->out.len)
            ws_flush(ws);
        return;
    }

    ws_compact_in(ws);
    str_push_len(&ws->in, data, len);

    ws->in_recv = true;
    int rc = wslay_event_recv(ws->ctx);
    ws->in_recv = false;
    if (ws->in_pos >= ws->in.len) {
        str_clear(&ws->in);
        ws->in_pos = 0;
    } else if (ws->in_pos > 65536) {
        ws_compact_in(ws);
    }

    if (rc != 0) {
        http_log("websocket recv error: %d", rc);
        ws->closing = true;
        ws->close_after_write = true;
    }
    if (ws->closing)
        wslay_event_shutdown_read(ws->ctx);

    int snd = wslay_event_send(ws->ctx);
    if (snd != 0) {
        http_log("websocket send error: %d", snd);
        ws->closing = true;
        ws->close_after_write = true;
    }
    if (wslay_event_get_close_sent(ws->ctx)) {
        ws->closing = true;
        ws->close_after_write = true;
        wslay_event_shutdown_read(ws->ctx);
    }
    ws_flush(ws);
}

void http_ws_disconnect(http_conn_t *c) {
    if (!c || !c->ws) return;
    http_ws_t *ws = c->ws;
    ws->closing = true;
    if (ws->ctx) {
        wslay_event_shutdown_read(ws->ctx);
        wslay_event_shutdown_write(ws->ctx);
    }
    ws_notify_close(ws, 1006, "", 0);
}

void http_ws_cleanup(http_conn_t *c) {
    if (!c || !c->ws) return;
    http_ws_t *ws = c->ws;
    c->ws = NULL;
    ws->conn = NULL;
    ws->closing = true;
    if (ws->writing) {
        ws->cleanup_pending = true;
        return;
    }
    ws_cleanup_now(ws);
}

static http_ws_t *ws_check(lua_State *L) {
    http_ws_t *ws = (http_ws_t *)luaL_checkudata(L, 1, HTTP_WS_MT);
    if (!ws->conn || !ws->ctx)
        luaL_error(L, "websocket is closed");
    return ws;
}

static int l_ws_on(lua_State *L) {
    http_ws_t *ws = ws_check(L);
    const char *event = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    int *slot;
    if (strcmp(event, "message") == 0)
        slot = &ws->on_message_ref;
    else if (strcmp(event, "close") == 0)
        slot = &ws->on_close_ref;
    else
        return luaL_error(L, "unknown websocket event '%s'", event);

    if (*slot != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, *slot);
    lua_pushvalue(L, 3);
    *slot = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, 1);
    return 1;
}

static int ws_send_msg(lua_State *L, uint8_t opcode) {
    http_ws_t *ws = ws_check(L);
    size_t len = 0;
    const char *data = lua_isnoneornil(L, 2)
        ? "" : luaL_checklstring(L, 2, &len);
    struct wslay_event_msg msg = {
        .opcode = opcode,
        .msg = (const uint8_t *)data,
        .msg_length = len,
    };
    int rc = wslay_event_queue_msg(ws->ctx, &msg);
    if (rc != 0)
        return luaL_error(L, "websocket send failed: %d", rc);
    if (!ws->in_recv)
        rc = wslay_event_send(ws->ctx);
    if (rc != 0) {
        ws->closing = true;
        ws->close_after_write = true;
    }
    ws_flush(ws);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_ws_send(lua_State *L) {
    uint8_t opcode = lua_toboolean(L, 3) ? WSLAY_BINARY_FRAME
                                         : WSLAY_TEXT_FRAME;
    return ws_send_msg(L, opcode);
}

static int l_ws_ping(lua_State *L) {
    return ws_send_msg(L, WSLAY_PING);
}

static int l_ws_close(lua_State *L) {
    http_ws_t *ws = ws_check(L);
    int code = (int)luaL_optinteger(L, 2, 1000);
    size_t rlen = 0;
    const char *reason = lua_isnoneornil(L, 3)
        ? "" : luaL_checklstring(L, 3, &rlen);

    if (rlen > 123)
        return luaL_error(L, "websocket close reason is too long");

    if (ws->closing) {
        lua_pushboolean(L, 1);
        return 1;
    }
    ws->closing = true;
    ws->close_after_write = true;
    wslay_event_shutdown_read(ws->ctx);
    ws_notify_close(ws, (uint16_t)code, reason, rlen);
    int rc = wslay_event_queue_close(ws->ctx, (uint16_t)code,
                                     (const uint8_t *)reason, rlen);
    if (rc == 0 && !ws->in_recv)
        rc = wslay_event_send(ws->ctx);
    if (rc != 0) {
        http_log("websocket close failed: %d", rc);
    }
    ws_flush(ws);
    lua_settop(L, 1);
    return 1;
}

static int l_ws_is_open(lua_State *L) {
    http_ws_t *ws = (http_ws_t *)luaL_checkudata(L, 1, HTTP_WS_MT);
    lua_pushboolean(L, ws->conn && ws->ctx && !ws->closing);
    return 1;
}

static int l_ws_gc(lua_State *L) {
    http_ws_t *ws = (http_ws_t *)luaL_checkudata(L, 1, HTTP_WS_MT);
    ws_cleanup_now(ws);
    return 0;
}

static void ws_ensure_metatable(lua_State *L) {
    if (luaL_newmetatable(L, HTTP_WS_MT)) {
        lua_newtable(L);
        lua_pushcfunction(L, l_ws_on);
        lua_setfield(L, -2, "on");
        lua_pushcfunction(L, l_ws_send);
        lua_setfield(L, -2, "send");
        lua_pushcfunction(L, l_ws_ping);
        lua_setfield(L, -2, "ping");
        lua_pushcfunction(L, l_ws_close);
        lua_setfield(L, -2, "close");
        lua_pushcfunction(L, l_ws_is_open);
        lua_setfield(L, -2, "is_open");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_ws_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

bool http_ws_init(http_conn_t *c, lua_State *L, http_route_t *route,
                  int req_idx) {
    if (!c || !L || !route || c->closing) return false;
    int req = req_idx < 0 ? lua_gettop(L) + req_idx + 1 : req_idx;

    if (!c->method || strcmp(c->method, "GET") != 0) {
        ws_reject(c, 400, "WebSocket upgrade requires GET");
        return false;
    }
    if (llhttp_get_http_major(&c->parser) != 1 ||
        llhttp_get_http_minor(&c->parser) < 1) {
        ws_reject(c, 400, "WebSocket upgrade requires HTTP/1.1");
        return false;
    }
    if (!headers_have_token(c->headers, c->nheaders, "upgrade", "websocket")) {
        ws_reject(c, 400, "Missing or invalid Upgrade header");
        return false;
    }
    if (!headers_have_token(c->headers, c->nheaders, "connection", "upgrade")) {
        ws_reject(c, 400, "Missing or invalid Connection header");
        return false;
    }

    char ver[16];
    if (!trim_copy(http_header_get(c->headers, c->nheaders,
                                   "sec-websocket-version"),
                   ver, sizeof(ver)) || strcmp(ver, "13") != 0) {
        ws_reject(c, 426, "Unsupported WebSocket version");
        return false;
    }

    char key[128];
    uint8_t key_bytes[24];
    if (!trim_copy(http_header_get(c->headers, c->nheaders,
                                   "sec-websocket-key"),
                   key, sizeof(key)) ||
        b64_decode(key, key_bytes, sizeof(key_bytes)) != 16) {
        ws_reject(c, 400, "Missing or invalid Sec-WebSocket-Key");
        return false;
    }

    char accept_in[sizeof(key) + sizeof(WS_GUID)];
    snprintf(accept_in, sizeof(accept_in), "%s%s", key, WS_GUID);
    sha1_ctx sha;
    uint8_t digest[20];
    char accept[32];
    sha1_init(&sha);
    sha1_update(&sha, (const uint8_t *)accept_in, strlen(accept_in));
    sha1_final(&sha, digest);
    b64_encode(digest, sizeof(digest), accept);

    int top = lua_gettop(L);
    http_ws_t *ws = (http_ws_t *)lua_newuserdata(L, sizeof(*ws));
    memset(ws, 0, sizeof(*ws));
    ws->conn = c;
    ws->L = L;
    ws->self_ref = LUA_NOREF;
    ws->on_message_ref = LUA_NOREF;
    ws->on_close_ref = LUA_NOREF;
    ws->in = str_init(1024);
    ws->out = str_init(1024);
    ws_ensure_metatable(L);
    luaL_getmetatable(L, HTTP_WS_MT);
    lua_setmetatable(L, -2);

    int ws_idx = lua_gettop(L);
    lua_pushvalue(L, ws_idx);
    ws->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    c->ws = ws;

    static const struct wslay_event_callbacks callbacks = {
        .recv_callback = ws_recv_cb,
        .send_callback = ws_send_cb,
        .genmask_callback = NULL,
        .on_frame_recv_start_callback = NULL,
        .on_frame_recv_chunk_callback = NULL,
        .on_frame_recv_end_callback = NULL,
        .on_msg_recv_callback = ws_on_msg_recv,
    };
    if (wslay_event_context_server_init(&ws->ctx, &callbacks, ws) != 0) {
        lua_settop(L, top);
        http_ws_cleanup(c);
        ws_reject(c, 500, "Failed to initialize WebSocket");
        return false;
    }
    if (c->state->cfg.max_ws_message)
        wslay_event_config_set_max_recv_msg_length(ws->ctx,
                                                   c->state->cfg.max_ws_message);

    str_push(&ws->out,
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: ");
    str_push(&ws->out, accept);
    str_push(&ws->out, "\r\n\r\n");

    lua_pushcfunction(L, ws_msghandler);
    int errfunc = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, route->handler_ref);
    lua_pushvalue(L, ws_idx);
    lua_pushvalue(L, req);
    ws->in_open_handler = true;
    int rc = lua_pcall(L, 2, 0, errfunc);
    ws->in_open_handler = false;
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        http_log("websocket handler error: %s",
                 msg ? msg : "(non-string error)");
        lua_settop(L, top);
        http_ws_cleanup(c);
        ws_reject(c, 500, "Internal Server Error");
        return false;
    }

    lua_settop(L, top);
    if (c->close_after_write)
        ws->close_after_write = true;
    ws_flush(ws);
    return true;
}
