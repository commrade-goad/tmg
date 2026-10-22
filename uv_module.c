#include "uv_module.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include <lauxlib.h>
#include <lua.h>

#define UV_HANDLE_MT "tmg.uv.handle"

typedef enum {
    UV_WRAP_TIMER = 1,
    UV_WRAP_TCP = 2
} uv_wrap_kind_t;

typedef struct uv_wrap uv_wrap_t;
typedef struct uv_write_ctx uv_write_ctx_t;

struct uv_write_ctx {
    uv_write_t req;
    uv_wrap_t *wrap;
    char *data;
    int cb_ref;
    bool shutdown;
    uv_write_ctx_t *next;
};

struct uv_wrap {
    uv_wrap_kind_t kind;
    lua_State *L;
    uv_loop_t *loop;
    union {
        uv_timer_t timer;
        uv_tcp_t tcp;
    } h;
    uv_connect_t connect_req;
    uv_write_ctx_t *writes;
    uv_wrap_t *next;
    int timer_ref;
    int listen_ref;
    int read_ref;
    int connect_ref;
    int close_ref;
    int self_ref;
    int pending_ops;
    bool user_alive;
    bool handle_closed;
    bool closing;
    bool shutdown;
    bool reading;
    bool listening;
    bool connected;
    bool connecting;
};

typedef struct {
    uv_wrap_t *wrap;
} uv_ud_t;

static uv_wrap_t *g_wraps;

static void report_callback_error(lua_State *L, const char *where) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "uv: %s callback error: %s\n", where, msg ? msg : "unknown error");
    lua_pop(L, 1);
}

static void replace_ref(lua_State *L, int *slot, int idx) {
    if (*slot != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, *slot);
        *slot = LUA_NOREF;
    }
    if (idx > 0 && !lua_isnil(L, idx)) {
        lua_pushvalue(L, idx);
        *slot = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static void clear_ref(lua_State *L, int *slot) {
    if (*slot != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, *slot);
        *slot = LUA_NOREF;
    }
}

static uv_handle_t *wrap_handle(uv_wrap_t *w) {
    if (w->kind == UV_WRAP_TIMER)
        return (uv_handle_t *)&w->h.timer;
    return (uv_handle_t *)&w->h.tcp;
}

static void wrap_free(uv_wrap_t *w) {
    if (!w) return;
    uv_wrap_t **pp = &g_wraps;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
        pp = &(*pp)->next;
    }
    free(w);
}

static void wrap_maybe_free(uv_wrap_t *w) {
    if (!w) return;
    if (!w->user_alive && w->handle_closed &&
        w->pending_ops == 0 && w->writes == NULL)
        wrap_free(w);
}

static uv_wrap_t *wrap_new(lua_State *L, uv_wrap_kind_t kind) {
    uv_wrap_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->kind = kind;
    w->L = L;
    w->loop = uv_default_loop();
    w->timer_ref = LUA_NOREF;
    w->listen_ref = LUA_NOREF;
    w->read_ref = LUA_NOREF;
    w->connect_ref = LUA_NOREF;
    w->close_ref = LUA_NOREF;
    w->self_ref = LUA_NOREF;
    w->user_alive = true;
    w->next = g_wraps;
    g_wraps = w;
    return w;
}

static void pending_add(lua_State *L, uv_wrap_t *w) {
    if (w->pending_ops++ == 0) {
        lua_pushvalue(L, 1);
        w->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static void pending_del(uv_wrap_t *w) {
    if (w->pending_ops > 0)
        w->pending_ops--;
    if (w->pending_ops == 0)
        clear_ref(w->L, &w->self_ref);
    wrap_maybe_free(w);
}

static int push_wrap(lua_State *L, uv_wrap_t *w) {
    uv_ud_t *ud = (uv_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->wrap = w;
    luaL_getmetatable(L, UV_HANDLE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static uv_wrap_t *check_handle(lua_State *L) {
    uv_ud_t *ud = (uv_ud_t *)luaL_checkudata(L, 1, UV_HANDLE_MT);
    if (!ud->wrap)
        luaL_error(L, "uv: handle is closed");
    return ud->wrap;
}

static uv_wrap_t *check_open(lua_State *L) {
    uv_wrap_t *w = check_handle(L);
    if (w->closing || w->handle_closed)
        luaL_error(L, "uv: handle is closed");
    return w;
}

static uv_wrap_t *check_kind(lua_State *L, uv_wrap_kind_t kind) {
    uv_wrap_t *w = check_open(L);
    if (w->kind != kind)
        luaL_error(L, "uv: wrong handle type");
    return w;
}

static bool push_address(lua_State *L, const struct sockaddr *sa) {
    char ip[64];
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        if (uv_ip4_name(a, ip, sizeof(ip)) != 0) return false;
        port = ntohs(a->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        if (uv_ip6_name(a, ip, sizeof(ip)) != 0) return false;
        port = ntohs(a->sin6_port);
    } else {
        return false;
    }
    lua_pushstring(L, ip);
    lua_pushinteger(L, port);
    return true;
}

static int make_address(const char *host, int port,
                        struct sockaddr_storage *ss, int *len) {
    if (!host || strcmp(host, "*") == 0 || host[0] == '\0')
        host = "0.0.0.0";
    else if (strcmp(host, "localhost") == 0)
        host = "127.0.0.1";

    memset(ss, 0, sizeof(*ss));
    if (strchr(host, ':')) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;
        int rc = uv_ip6_addr(host, port, a);
        if (rc != 0) return rc;
        *len = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)ss;
        int rc = uv_ip4_addr(host, port, a);
        if (rc != 0) return rc;
        *len = sizeof(*a);
    }
    return 0;
}

static void on_handle_close(uv_handle_t *h) {
    uv_wrap_t *w = (uv_wrap_t *)h->data;
    if (!w) return;

    lua_State *L = w->L;
    int cb = w->close_ref;
    w->close_ref = LUA_NOREF;

    clear_ref(L, &w->listen_ref);
    clear_ref(L, &w->read_ref);
    clear_ref(L, &w->timer_ref);
    clear_ref(L, &w->connect_ref);

    for (uv_write_ctx_t *ctx = w->writes; ctx; ctx = ctx->next) {
        ctx->shutdown = true;
        clear_ref(L, &ctx->cb_ref);
    }

    w->reading = false;
    w->listening = false;
    w->connecting = false;
    w->handle_closed = true;

    if (!w->shutdown && cb != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, cb);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
            report_callback_error(L, "close");
    }
    clear_ref(L, &cb);
    wrap_maybe_free(w);
}

static int l_handle_close(lua_State *L) {
    uv_wrap_t *w = check_handle(L);
    if (w->closing || w->handle_closed) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    replace_ref(L, &w->close_ref, lua_isnoneornil(L, 2) ? 0 : 2);
    if (!w->closing) {
        w->closing = true;
        uv_close(wrap_handle(w), on_handle_close);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_handle_is_closing(lua_State *L) {
    uv_wrap_t *w = check_handle(L);
    lua_pushboolean(L, w->closing || w->handle_closed);
    return 1;
}

static int l_handle_is_active(lua_State *L) {
    uv_wrap_t *w = check_handle(L);
    lua_pushboolean(L, !w->handle_closed && uv_is_active(wrap_handle(w)));
    return 1;
}

static int l_handle_is_reading(lua_State *L) {
    uv_wrap_t *w = check_handle(L);
    lua_pushboolean(L, w->reading);
    return 1;
}

static void on_timer(uv_timer_t *h) {
    uv_wrap_t *w = (uv_wrap_t *)h->data;
    if (!w || w->shutdown || w->handle_closed) return;
    int ref = w->timer_ref;
    if (ref == LUA_NOREF) return;

    lua_State *L = w->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        report_callback_error(L, "timer");
        uv_timer_stop(&w->h.timer);
    }
}

static int l_timer_start(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TIMER);
    lua_Integer timeout = luaL_checkinteger(L, 2);
    lua_Integer repeat = luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    if (timeout < 0 || repeat < 0)
        luaL_error(L, "uv.timer:start expects non-negative milliseconds");

    int old = w->timer_ref;
    w->timer_ref = LUA_NOREF;
    replace_ref(L, &w->timer_ref, 4);
    int rc = uv_timer_start(&w->h.timer, on_timer,
                            (uint64_t)timeout, (uint64_t)repeat);
    if (rc != 0) {
        clear_ref(L, &w->timer_ref);
        w->timer_ref = old;
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    clear_ref(L, &old);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_timer_stop(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TIMER);
    uv_timer_stop(&w->h.timer);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_timer_again(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TIMER);
    int rc = uv_timer_again(&w->h.timer);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static void on_connect(uv_connect_t *req, int status) {
    uv_wrap_t *w = (uv_wrap_t *)req->data;
    if (!w) return;
    lua_State *L = w->L;
    int ref = w->connect_ref;
    w->connect_ref = LUA_NOREF;
    w->connecting = false;
    w->connected = status == 0;

    if (!w->shutdown && !w->handle_closed && ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (status < 0)
            lua_pushstring(L, uv_strerror(status));
        else
            lua_pushnil(L);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            report_callback_error(L, "connect");
    }
    clear_ref(L, &ref);
    pending_del(w);
}

static int l_tcp_connect(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    const char *host = luaL_checkstring(L, 2);
    lua_Integer port = luaL_checkinteger(L, 3);
    if (port < 0 || port > 65535)
        luaL_error(L, "uv.tcp:connect invalid port");
    if (!lua_isnoneornil(L, 4))
        luaL_checktype(L, 4, LUA_TFUNCTION);

    struct sockaddr_storage ss;
    int len = 0;
    int rc = make_address(host, (int)port, &ss, &len);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }

    if (w->connected || w->connecting || w->listening) {
        lua_pushnil(L);
        lua_pushstring(L, "tcp handle is already connected, connecting, or listening");
        return 2;
    }

    replace_ref(L, &w->connect_ref, lua_isnoneornil(L, 4) ? 0 : 4);
    w->connect_req.data = w;
    w->connecting = true;
    pending_add(L, w);
    rc = uv_tcp_connect(&w->connect_req, &w->h.tcp,
                        (const struct sockaddr *)&ss, on_connect);
    if (rc != 0) {
        w->connecting = false;
        pending_del(w);
        clear_ref(L, &w->connect_ref);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    (void)h;
    size_t len = suggested ? suggested : 65536;
    buf->base = (char *)malloc(len);
    buf->len = buf->base ? len : 0;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    uv_wrap_t *w = (uv_wrap_t *)stream->data;
    if (!w) {
        free(buf->base);
        return;
    }
    lua_State *L = w->L;

    if (nread > 0) {
        if (!w->shutdown && !w->handle_closed && w->read_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, w->read_ref);
            lua_pushnil(L);
            lua_pushlstring(L, buf->base, (size_t)nread);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                report_callback_error(L, "read");
                uv_read_stop(stream);
                w->reading = false;
            }
        }
    } else if (nread < 0) {
        w->reading = false;
        int ref = w->read_ref;
        w->read_ref = LUA_NOREF;
        if (!w->shutdown && !w->handle_closed && ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (nread == UV_EOF)
                lua_pushstring(L, "EOF");
            else
                lua_pushstring(L, uv_strerror((int)nread));
            if (lua_pcall(L, 1, 0, 0) != LUA_OK)
                report_callback_error(L, "read");
        }
        clear_ref(L, &ref);
    }
    free(buf->base);
}

static int l_tcp_read_start(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int old = w->read_ref;
    w->read_ref = LUA_NOREF;
    replace_ref(L, &w->read_ref, 2);
    if (w->reading) {
        clear_ref(L, &old);
        lua_pushboolean(L, 1);
        return 1;
    }

    int rc = uv_read_start((uv_stream_t *)&w->h.tcp, on_alloc, on_read);
    if (rc != 0) {
        clear_ref(L, &w->read_ref);
        w->read_ref = old;
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    clear_ref(L, &old);
    w->reading = true;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tcp_read_stop(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    uv_read_stop((uv_stream_t *)&w->h.tcp);
    w->reading = false;
    clear_ref(L, &w->read_ref);
    lua_pushboolean(L, 1);
    return 1;
}

static void write_remove(uv_wrap_t *w, uv_write_ctx_t *ctx) {
    uv_write_ctx_t **pp = &w->writes;
    while (*pp) {
        if (*pp == ctx) {
            *pp = ctx->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void on_write(uv_write_t *req, int status) {
    uv_write_ctx_t *ctx = (uv_write_ctx_t *)req->data;
    if (!ctx) return;
    uv_wrap_t *w = ctx->wrap;
    lua_State *L = w->L;
    int ref = ctx->cb_ref;
    bool shutdown = ctx->shutdown || w->shutdown || w->handle_closed;

    write_remove(w, ctx);
    free(ctx->data);

    if (!shutdown && ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (status < 0)
            lua_pushstring(L, uv_strerror(status));
        else
            lua_pushnil(L);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            report_callback_error(L, "write");
    }
    clear_ref(L, &ref);
    free(ctx);
    pending_del(w);
}

static int l_tcp_write(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len > UINT_MAX) {
        lua_pushnil(L);
        lua_pushstring(L, "write exceeds libuv buffer limit");
        return 2;
    }
    if (!lua_isnoneornil(L, 3))
        luaL_checktype(L, 3, LUA_TFUNCTION);

    uv_write_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return luaL_error(L, "uv.tcp:write out of memory");
    ctx->data = malloc(len ? len : 1);
    if (!ctx->data) {
        free(ctx);
        return luaL_error(L, "uv.tcp:write out of memory");
    }
    if (len) memcpy(ctx->data, data, len);
    ctx->wrap = w;
    ctx->cb_ref = LUA_NOREF;
    replace_ref(L, &ctx->cb_ref, lua_isnoneornil(L, 3) ? 0 : 3);

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)len);

    ctx->req.data = ctx;
    ctx->next = w->writes;
    w->writes = ctx;
    pending_add(L, w);
    int rc = uv_write(&ctx->req, (uv_stream_t *)&w->h.tcp, &buf, 1, on_write);
    if (rc != 0) {
        write_remove(w, ctx);
        clear_ref(L, &ctx->cb_ref);
        free(ctx->data);
        free(ctx);
        pending_del(w);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static void close_client(uv_wrap_t *w) {
    if (w && !w->closing && !w->handle_closed) {
        w->closing = true;
        uv_close(wrap_handle(w), on_handle_close);
    }
}

static void on_connection(uv_stream_t *server, int status) {
    uv_wrap_t *sw = (uv_wrap_t *)server->data;
    if (!sw || sw->shutdown) return;
    lua_State *L = sw->L;
    int ref = sw->listen_ref;
    if (ref == LUA_NOREF) return;

    if (status < 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(status));
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            report_callback_error(L, "listen");
        return;
    }

    uv_wrap_t *cw = wrap_new(L, UV_WRAP_TCP);
    if (!cw) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            report_callback_error(L, "listen");
        return;
    }
    cw->h.tcp.data = cw;
    int rc = uv_tcp_init(sw->loop, &cw->h.tcp);
    if (rc != 0) {
        cw->user_alive = false;
        cw->handle_closed = true;
        wrap_free(cw);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            report_callback_error(L, "listen");
        return;
    }
    rc = uv_accept(server, (uv_stream_t *)&cw->h.tcp);
    if (rc != 0) {
        cw->user_alive = false;
        cw->closing = true;
        uv_close((uv_handle_t *)&cw->h.tcp, on_handle_close);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            report_callback_error(L, "listen");
        return;
    }
    cw->connected = true;

    push_wrap(L, cw);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushvalue(L, -2);
    lua_pushnil(L);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        report_callback_error(L, "listen");
        close_client(cw);
    }
    lua_pop(L, 1);
}

static int l_tcp_listen(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    lua_Integer backlog = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (backlog < 0 || backlog > INT_MAX)
        luaL_error(L, "uv.tcp:listen invalid backlog");

    replace_ref(L, &w->listen_ref, 3);
    if (w->listening) {
        lua_pushboolean(L, 1);
        return 1;
    }
    int rc = uv_listen((uv_stream_t *)&w->h.tcp, (int)backlog, on_connection);
    if (rc != 0) {
        clear_ref(L, &w->listen_ref);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    w->listening = true;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tcp_bind(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    const char *host = luaL_checkstring(L, 2);
    lua_Integer port = luaL_checkinteger(L, 3);
    if (port < 0 || port > 65535)
        luaL_error(L, "uv.tcp:bind invalid port");

    struct sockaddr_storage ss;
    int len = 0;
    int rc = make_address(host, (int)port, &ss, &len);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    rc = uv_tcp_bind(&w->h.tcp, (const struct sockaddr *)&ss, 0);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tcp_getsockname(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    struct sockaddr_storage ss;
    int len = sizeof(ss);
    int rc = uv_tcp_getsockname(&w->h.tcp, (struct sockaddr *)&ss, &len);
    if (rc != 0 || !push_address(L, (struct sockaddr *)&ss)) {
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    return 2;
}

static int l_tcp_getpeername(lua_State *L) {
    uv_wrap_t *w = check_kind(L, UV_WRAP_TCP);
    struct sockaddr_storage ss;
    int len = sizeof(ss);
    int rc = uv_tcp_getpeername(&w->h.tcp, (struct sockaddr *)&ss, &len);
    if (rc != 0 || !push_address(L, (struct sockaddr *)&ss)) {
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    return 2;
}

static int uv_gc(lua_State *L) {
    uv_ud_t *ud = (uv_ud_t *)luaL_checkudata(L, 1, UV_HANDLE_MT);
    if (!ud->wrap) return 0;
    uv_wrap_t *w = ud->wrap;
    ud->wrap = NULL;
    w->user_alive = false;
    if (!w->handle_closed && !w->closing) {
        w->closing = true;
        uv_close(wrap_handle(w), on_handle_close);
    }
    wrap_maybe_free(w);
    return 0;
}

static int l_uv_timer(lua_State *L) {
    uv_wrap_t *w = wrap_new(L, UV_WRAP_TIMER);
    if (!w) return luaL_error(L, "uv.timer out of memory");
    w->h.timer.data = w;
    int rc = uv_timer_init(w->loop, &w->h.timer);
    if (rc != 0) {
        w->handle_closed = true;
        w->user_alive = false;
        wrap_free(w);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    return push_wrap(L, w);
}

static int l_uv_tcp(lua_State *L) {
    uv_wrap_t *w = wrap_new(L, UV_WRAP_TCP);
    if (!w) return luaL_error(L, "uv.tcp out of memory");
    w->h.tcp.data = w;
    int rc = uv_tcp_init(w->loop, &w->h.tcp);
    if (rc != 0) {
        w->handle_closed = true;
        w->user_alive = false;
        wrap_free(w);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    return push_wrap(L, w);
}

static int l_uv_run(lua_State *L) {
    uv_run_mode mode = UV_RUN_DEFAULT;
    if (!lua_isnoneornil(L, 1)) {
        const char *m = luaL_checkstring(L, 1);
        if (strcmp(m, "default") == 0)
            mode = UV_RUN_DEFAULT;
        else if (strcmp(m, "once") == 0)
            mode = UV_RUN_ONCE;
        else if (strcmp(m, "nowait") == 0)
            mode = UV_RUN_NOWAIT;
        else
            return luaL_error(L, "uv.run invalid mode: %s", m);
    }
    int rc = uv_run(uv_default_loop(), mode);
    lua_pushboolean(L, rc != 0);
    return 1;
}

static int l_uv_stop(lua_State *L) {
    uv_stop(uv_default_loop());
    lua_pushboolean(L, 1);
    return 1;
}

static int l_uv_loop_alive(lua_State *L) {
    lua_pushboolean(L, uv_loop_alive(uv_default_loop()));
    return 1;
}

static int l_uv_now(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)uv_now(uv_default_loop()));
    return 1;
}

static int l_uv_update_time(lua_State *L) {
    uv_update_time(uv_default_loop());
    lua_pushboolean(L, 1);
    return 1;
}

static int l_uv_hrtime(lua_State *L) {
    lua_pushnumber(L, (lua_Number)uv_hrtime());
    return 1;
}

static int l_uv_strerror(lua_State *L) {
    int rc = (int)luaL_checkinteger(L, 1);
    lua_pushstring(L, uv_strerror(rc));
    return 1;
}

static int l_uv_version(lua_State *L) {
    lua_pushinteger(L, uv_version());
    return 1;
}

static int l_uv_version_string(lua_State *L) {
    lua_pushstring(L, uv_version_string());
    return 1;
}

void tmg_uv_shutdown(lua_State *L) {
    tmg_uv_fs_shutdown(L);
    uv_wrap_t *w = g_wraps;
    while (w) {
        uv_wrap_t *next = w->next;
        w->shutdown = true;
        clear_ref(w->L, &w->timer_ref);
        clear_ref(w->L, &w->listen_ref);
        clear_ref(w->L, &w->read_ref);
        clear_ref(w->L, &w->connect_ref);
        clear_ref(w->L, &w->close_ref);
        for (uv_write_ctx_t *ctx = w->writes; ctx; ctx = ctx->next) {
            ctx->shutdown = true;
            clear_ref(w->L, &ctx->cb_ref);
        }
        if (!w->closing && !w->handle_closed) {
            w->closing = true;
            uv_close(wrap_handle(w), on_handle_close);
        }
        w = next;
    }
}

static const luaL_Reg uv_lib[] = {
    {"run", l_uv_run},
    {"stop", l_uv_stop},
    {"loop_alive", l_uv_loop_alive},
    {"now", l_uv_now},
    {"update_time", l_uv_update_time},
    {"hrtime", l_uv_hrtime},
    {"strerror", l_uv_strerror},
    {"version", l_uv_version},
    {"version_string", l_uv_version_string},
    {"timer", l_uv_timer},
    {"new_timer", l_uv_timer},
    {"tcp", l_uv_tcp},
    {"new_tcp", l_uv_tcp},
    {NULL, NULL}
};

static const luaL_Reg uv_methods[] = {
    {"start", l_timer_start},
    {"stop", l_timer_stop},
    {"again", l_timer_again},
    {"bind", l_tcp_bind},
    {"listen", l_tcp_listen},
    {"connect", l_tcp_connect},
    {"read_start", l_tcp_read_start},
    {"read_stop", l_tcp_read_stop},
    {"write", l_tcp_write},
    {"getsockname", l_tcp_getsockname},
    {"getpeername", l_tcp_getpeername},
    {"close", l_handle_close},
    {"is_closing", l_handle_is_closing},
    {"is_active", l_handle_is_active},
    {"is_reading", l_handle_is_reading},
    {NULL, NULL}
};

int luaopen_uv(lua_State *L) {
    if (luaL_newmetatable(L, UV_HANDLE_MT)) {
        lua_pushcfunction(L, uv_gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        luaL_register(L, NULL, uv_methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    luaL_newlib(L, uv_lib);
    tmg_uv_fs_register(L);
    return 1;
}
