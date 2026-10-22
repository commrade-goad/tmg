#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void state_defaults(http_state_t *st) {
    memset(st, 0, sizeof(*st));
    st->cfg.host = http_strdup("127.0.0.1");
    st->cfg.port = 8080;
    st->cfg.max_body = 10 * 1024 * 1024;
    st->cfg.max_header_bytes = 64 * 1024;
    st->cfg.max_headers = 100;
    st->cfg.max_url = 8192;
    st->cfg.header_timeout = 10;
    st->cfg.keepalive_timeout = 15;
    st->cfg.upload_dir = http_strdup("uploads");
    st->cfg.template_root = http_strdup(".");
    st->cfg.debug = false;
}

static http_state_t *ensure_state(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, HTTP_STATE_KEY);
    http_state_t *st = lua_isuserdata(L, -1) ? (http_state_t *)lua_touserdata(L, -1) : NULL;
    if (st) {
        lua_pop(L, 1);
        return st;
    }
    lua_pop(L, 1);

    st = (http_state_t *)lua_newuserdata(L, sizeof(*st));
    state_defaults(st);
    st->L = L;
    lua_setfield(L, LUA_REGISTRYINDEX, HTTP_STATE_KEY);
    return st;
}

static int l_http_listen(lua_State *L) {
    http_state_t *st = ensure_state(L);
    if (st->listening)
        return luaL_error(L, "http.listen: server is already running");

    if (!lua_isnoneornil(L, 1))
        luaL_checktype(L, 1, LUA_TTABLE);

    free(st->cfg.host);
    free(st->cfg.upload_dir);
    free(st->cfg.template_root);
    st->cfg.host = http_strdup("127.0.0.1");
    st->cfg.port = 8080;
    st->cfg.max_body = 10 * 1024 * 1024;
    st->cfg.max_header_bytes = 64 * 1024;
    st->cfg.max_headers = 100;
    st->cfg.max_url = 8192;
    st->cfg.header_timeout = 10;
    st->cfg.keepalive_timeout = 15;
    st->cfg.upload_dir = http_strdup("uploads");
    st->cfg.template_root = http_strdup(".");
    st->cfg.debug = false;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "host");
        if (lua_isstring(L, -1)) {
            free(st->cfg.host);
            st->cfg.host = http_strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "port");
        if (lua_isnumber(L, -1)) {
            lua_Integer p = lua_tointeger(L, -1);
            if (p < 0 || p > 65535)
                return luaL_error(L, "http.listen: invalid port");
            st->cfg.port = (int)p;
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "max_body");
        if (lua_isnumber(L, -1)) st->cfg.max_body = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "max_header_bytes");
        if (lua_isnumber(L, -1)) st->cfg.max_header_bytes = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "max_headers");
        if (lua_isnumber(L, -1)) st->cfg.max_headers = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "max_url");
        if (lua_isnumber(L, -1)) st->cfg.max_url = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "header_timeout");
        if (lua_isnumber(L, -1)) st->cfg.header_timeout = (unsigned)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "keepalive_timeout");
        if (lua_isnumber(L, -1)) st->cfg.keepalive_timeout = (unsigned)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "upload_dir");
        if (lua_isstring(L, -1)) {
            free(st->cfg.upload_dir);
            st->cfg.upload_dir = http_strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "template_root");
        if (lua_isstring(L, -1)) {
            free(st->cfg.template_root);
            st->cfg.template_root = http_strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "debug");
        if (!lua_isnil(L, -1)) st->cfg.debug = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (st->cfg.max_header_bytes == 0) st->cfg.max_header_bytes = 64 * 1024;
    if (st->cfg.max_headers == 0) st->cfg.max_headers = 100;
    if (st->cfg.max_url == 0) st->cfg.max_url = 8192;

    int rc = http_state_start(L, st);
    if (rc != 0)
        return luaL_error(L, "http.listen: %s", uv_strerror(rc));

    lua_pushboolean(L, 1);
    return 1;
}

static int l_http_stop(lua_State *L) {
    http_state_t *st = ensure_state(L);
    http_state_stop(st);
    lua_pushboolean(L, 1);
    return 1;
}

static int route_common(lua_State *L, const char *method) {
    http_state_t *st = ensure_state(L);
    const char *pattern = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    str_t err = str_init(64);
    if (!http_route_add(st, method, pattern, ref, &err)) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        const char *msg = err.len ? err.data : "invalid route";
        str_deinit(&err);
        return luaL_error(L, "http.route: %s", msg);
    }
    str_deinit(&err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_http_get(lua_State *L) { return route_common(L, "GET"); }
static int l_http_post(lua_State *L) { return route_common(L, "POST"); }
static int l_http_put(lua_State *L) { return route_common(L, "PUT"); }
static int l_http_patch(lua_State *L) { return route_common(L, "PATCH"); }
static int l_http_delete(lua_State *L) { return route_common(L, "DELETE"); }
static int l_http_head(lua_State *L) { return route_common(L, "HEAD"); }
static int l_http_options(lua_State *L) { return route_common(L, "OPTIONS"); }

static int l_http_route(lua_State *L) {
    char *method = http_strdup(luaL_checkstring(L, 1));
    lua_remove(L, 1);
    int rc = route_common(L, method);
    free(method);
    return rc;
}

static int http_msghandler(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") &&
            lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)",
                              luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static void send_simple(http_conn_t *c, int status, const char *allow) {
    http_response_reset(c);
    c->status = status;
    http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                     "Content-Type", "text/plain; charset=utf-8", true);
    if (allow && *allow)
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Allow", allow, true);
    str_clear(&c->res_body);
    str_push(&c->res_body, http_status_reason(status));
    c->ended = true;
    http_conn_send_response(c);
}

int http_dispatch(http_conn_t *c) {
    if (!c || !c->state) return 0;
    http_state_t *st = c->state;
    lua_State *L = st->L;
    if (!L) return 0;

    const char *url = c->url.data ? c->url.data : "/";
    const char *q = strchr(url, '?');
    size_t pathlen = q ? (size_t)(q - url) : strlen(url);
    const char *query = q ? q + 1 : "";
    const char *method = c->method ? c->method : "GET";

    str_t path = str_init(pathlen + 1);
    http_percent_decode(url, pathlen, false, &path);
    if (path.len == 0)
        str_push_chr(&path, '/');
    if (memchr(path.data, '\0', path.len) != NULL) {
        send_simple(c, 400, NULL);
        str_deinit(&path);
        return 0;
    }
    char *raw_path = http_strndup(url, pathlen);

    http_route_t *route = http_route_match(st, method, path.data);
    if (!route && strcmp(method, "HEAD") == 0)
        route = http_route_match(st, "GET", path.data);

    if (!route) {
        str_t allow = str_init(32);
        bool is_405 = http_route_allowed(st, path.data, &allow);
        send_simple(c, is_405 ? 405 : 404, is_405 ? allow.data : NULL);
        str_deinit(&allow);
        str_deinit(&path);
        free(raw_path);
        return 0;
    }

    if (!http_build_request(L, c, path.data, query, raw_path, route)) {
        http_conn_send_error(c, 400, http_status_reason(400));
        str_deinit(&path);
        free(raw_path);
        return 0;
    }

    int base = lua_gettop(L); /* request table */
    lua_pushcfunction(L, http_msghandler);
    int errfunc = base + 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, route->handler_ref);
    lua_pushvalue(L, base);
    http_response_push(L, c);

    int rc = lua_pcall(L, 2, 0, errfunc);
    if (rc != LUA_OK) {
        str_t emsg = str_init(64);
        const char *msg = lua_tostring(L, -1);
        if (msg) str_push(&emsg, msg);
        http_log("handler error: %s", emsg.len ? emsg.data : "(non-string error)");
        lua_settop(L, base - 1);
        http_response_reset(c);
        const char *body = st->cfg.debug && emsg.len ? emsg.data : "Internal Server Error";
        http_conn_send_error(c, 500, body);
        str_deinit(&emsg);
    } else {
        lua_settop(L, base - 1);
        if (!c->ended) c->ended = true;
        http_conn_send_response(c);
    }

    str_deinit(&path);
    free(raw_path);
    return rc == LUA_OK;
}

static const luaL_Reg http_lib[] = {
    {"listen", l_http_listen},
    {"stop", l_http_stop},
    {"route", l_http_route},
    {"get", l_http_get},
    {"post", l_http_post},
    {"put", l_http_put},
    {"patch", l_http_patch},
    {"delete", l_http_delete},
    {"head", l_http_head},
    {"options", l_http_options},
    {NULL, NULL}
};

int luaopen_http(lua_State *L) {
    http_state_t *st = ensure_state(L);
    st->L = L;
    luaL_newlib(L, http_lib);
    return 1;
}
