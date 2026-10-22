#include "http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *lower_dup(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < n; i++)
        p[i] = (char)tolower((unsigned char)s[i]);
    p[n] = '\0';
    return p;
}

static void set_lstring_field(lua_State *L, int tidx, const char *key,
                              const char *val, size_t len) {
    lua_pushstring(L, key);
    lua_pushlstring(L, val, len);
    lua_settable(L, tidx);
}

static void build_headers_table(lua_State *L, http_conn_t *c) {
    lua_newtable(L);
    int tidx = lua_gettop(L);

    for (size_t i = 0; i < c->nheaders; i++) {
        char *name = lower_dup(c->headers[i].name);
        if (!name) continue;
        const char *value = c->headers[i].value ? c->headers[i].value : "";

        lua_getfield(L, tidx, name);
        if (lua_isstring(L, -1)) {
            const char *sep = (strcmp(name, "cookie") == 0) ? "; " : ", ";
            lua_pushvalue(L, -1);
            lua_pushstring(L, sep);
            lua_pushstring(L, value);
            lua_concat(L, 3);
            lua_setfield(L, tidx, name);
        } else {
            lua_pop(L, 1);
            lua_pushstring(L, value);
            lua_setfield(L, tidx, name);
        }
        free(name);
    }
}

static int l_req_header(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *name = luaL_checkstring(L, 2);
    char *ln = lower_dup(name);
    if (!ln) return luaL_error(L, "out of memory");

    lua_getfield(L, 1, "headers");
    if (!lua_istable(L, -1)) {
        free(ln);
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, ln);
    free(ln);
    lua_remove(L, -2);
    return 1;
}

static int l_req_query(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *name = luaL_checkstring(L, 2);
    lua_getfield(L, 1, "query_params");
    if (!lua_istable(L, -1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    return 1;
}

static int l_req_is_keep_alive(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "keep_alive");
    return 1;
}

static int l_req_remote_address(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "remote_address");
    return 1;
}

int http_request_push_metatable(lua_State *L) {
    if (luaL_newmetatable(L, HTTP_REQ_MT)) {
        lua_newtable(L);
        lua_pushcfunction(L, l_req_header);
        lua_setfield(L, -2, "header");
        lua_pushcfunction(L, l_req_query);
        lua_setfield(L, -2, "query");
        lua_pushcfunction(L, l_req_is_keep_alive);
        lua_setfield(L, -2, "is_keep_alive");
        lua_pushcfunction(L, l_req_remote_address);
        lua_setfield(L, -2, "remote_address");
        lua_setfield(L, -2, "__index");
    }
    return 1;
}

bool http_build_request(lua_State *L, http_conn_t *c, const char *path,
                        const char *query, const char *raw_path,
                        http_route_t *route) {
    if (!L || !c) return false;

    lua_newtable(L);
    int req = lua_gettop(L);

    set_lstring_field(L, req, "method", c->method ? c->method : "GET",
                      strlen(c->method ? c->method : "GET"));
    set_lstring_field(L, req, "url", c->url.data, c->url.len);
    set_lstring_field(L, req, "path", path, strlen(path));
    set_lstring_field(L, req, "query", query, strlen(query));
    if (raw_path) set_lstring_field(L, req, "raw_path", raw_path, strlen(raw_path));
    set_lstring_field(L, req, "version", c->version ? c->version : "HTTP/1.1",
                      strlen(c->version ? c->version : "HTTP/1.1"));
    lua_pushboolean(L, c->keep_alive);
    lua_setfield(L, req, "keep_alive");
    lua_pushstring(L, c->remote);
    lua_setfield(L, req, "remote_address");

    build_headers_table(L, c);
    lua_setfield(L, req, "headers");

    lua_newtable(L);
    int params = lua_gettop(L);
    if (route && route->dynamic)
        http_route_fill_params(route, path, L, params);
    lua_setfield(L, req, "params");

    lua_newtable(L);
    int qparams = lua_gettop(L);
    if (query && *query)
        http_parse_urlencoded(L, query, strlen(query), qparams);
    lua_setfield(L, req, "query_params");

    lua_newtable(L);
    int cookies = lua_gettop(L);
    const char *cookie = http_header_get(c->headers, c->nheaders, "cookie");
    if (cookie) http_cookies_parse(L, cookie, cookies);
    lua_setfield(L, req, "cookies");

    lua_pushlstring(L, c->body.data ? c->body.data : "", c->body.len);
    lua_setfield(L, req, "body");

    lua_newtable(L);
    int form = lua_gettop(L);
    lua_newtable(L);
    int files = lua_gettop(L);

    const char *ct = http_header_get(c->headers, c->nheaders, "content-type");
    bool ok = true;
    if (ct && c->body.len > 0) {
        if (strncasecmp(ct, "application/x-www-form-urlencoded", 33) == 0) {
            http_parse_urlencoded(L, c->body.data, c->body.len, form);
        } else if (strncasecmp(ct, "multipart/form-data", 19) == 0) {
            ok = http_multipart_parse(c, L, form, files);
        }
    }

    if (!ok) {
        lua_settop(L, req - 1);
        return false;
    }

    /* stack currently: req, form, files */
    lua_setfield(L, req, "files");
    lua_setfield(L, req, "form");

    http_request_push_metatable(L);
    lua_setmetatable(L, req);
    lua_settop(L, req);
    return true;
}
