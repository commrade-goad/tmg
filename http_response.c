#include "http.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    http_conn_t *conn;
} http_res_t;

static http_conn_t *check_conn(lua_State *L) {
    http_res_t *r = (http_res_t *)luaL_checkudata(L, 1, HTTP_RES_MT);
    if (!r->conn) luaL_error(L, "response is detached");
    return r->conn;
}

static bool token_char(unsigned char c) {
    if (isalnum(c)) return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

static bool valid_header_name(const char *s) {
    if (!s || !*s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!token_char(*p)) return false;
    return true;
}

static bool valid_header_value(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == 0x7f) return false;
        if (*p < 0x20 && *p != '\t') return false;
    }
    return true;
}

static int l_res_status(lua_State *L) {
    http_conn_t *c = check_conn(L);
    int status = (int)luaL_checkinteger(L, 2);
    if (c->ended) return luaL_error(L, "response already sent");
    if (status < 200 || status > 599)
        return luaL_error(L, "invalid HTTP status %d", status);
    c->status = status;
    lua_settop(L, 1);
    return 1;
}

static int l_res_header(lua_State *L) {
    http_conn_t *c = check_conn(L);
    const char *name = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);
    if (c->ended) return luaL_error(L, "response already sent");
    if (!valid_header_name(name))
        return luaL_error(L, "invalid header name");
    if (!valid_header_value(value))
        return luaL_error(L, "invalid header value");
    if (strcasecmp(name, "content-length") == 0 ||
        strcasecmp(name, "transfer-encoding") == 0 ||
        strcasecmp(name, "connection") == 0)
        return luaL_error(L, "header '%s' is managed by the server", name);

    if (!http_headers_add(&c->res_headers, &c->nres_headers,
                          &c->cres_headers, name, value, true))
        return luaL_error(L, "out of memory");
    lua_settop(L, 1);
    return 1;
}

static int l_res_send(lua_State *L) {
    http_conn_t *c = check_conn(L);
    if (c->ended) return luaL_error(L, "response already sent");
    size_t len = 0;
    const char *body = lua_isnoneornil(L, 2) ? "" : luaL_checklstring(L, 2, &len);
    str_clear(&c->res_body);
    str_push_len(&c->res_body, body, len);
    c->ended = true;
    lua_settop(L, 1);
    return 1;
}

static int l_res_json(lua_State *L) {
    http_conn_t *c = check_conn(L);
    if (c->ended) return luaL_error(L, "response already sent");
    luaL_checkany(L, 2);

    str_t out = str_init(128);
    if (!http_json_encode(L, 2, &out, 0)) {
        str_deinit(&out);
        luaL_error(L, "json: unsupported or too deeply nested value");
    }

    if (!http_header_get(c->res_headers, c->nres_headers, "content-type"))
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Content-Type", "application/json; charset=utf-8", true);

    str_clear(&c->res_body);
    str_push_len(&c->res_body, out.data, out.len);
    str_deinit(&out);
    c->ended = true;
    lua_settop(L, 1);
    return 1;
}

static int l_res_html(lua_State *L) {
    http_conn_t *c = check_conn(L);
    if (c->ended) return luaL_error(L, "response already sent");
    size_t len = 0;
    const char *body = luaL_checklstring(L, 2, &len);

    if (!http_header_get(c->res_headers, c->nres_headers, "content-type"))
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Content-Type", "text/html; charset=utf-8", true);

    str_clear(&c->res_body);
    str_push_len(&c->res_body, body, len);
    c->ended = true;
    lua_settop(L, 1);
    return 1;
}

static int l_res_redirect(lua_State *L) {
    http_conn_t *c = check_conn(L);
    const char *location = luaL_checkstring(L, 2);
    int status = (int)luaL_optinteger(L, 3, 302);
    if (c->ended) return luaL_error(L, "response already sent");
    if (status < 300 || status > 399)
        return luaL_error(L, "redirect status must be 3xx");
    if (!valid_header_value(location))
        return luaL_error(L, "invalid redirect location");

    c->status = status;
    http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                     "Location", location, true);
    str_clear(&c->res_body);
    c->ended = true;
    lua_settop(L, 1);
    return 1;
}

static int l_res_cookie(lua_State *L) {
    http_conn_t *c = check_conn(L);
    const char *name = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);
    if (c->ended) return luaL_error(L, "response already sent");
    if (!http_cookie_header(c, name, value, 4, L))
        return luaL_error(L, "failed to set cookie");
    lua_settop(L, 1);
    return 1;
}

static int l_res_render(lua_State *L) {
    http_conn_t *c = check_conn(L);
    const char *rel = luaL_checkstring(L, 2);
    if (c->ended) return luaL_error(L, "response already sent");

    const char *root = c->state->cfg.template_root;
    if (!root || !*root) root = ".";
    str_t full = str_init(128);
    if (!http_path_safe(root, rel, &full)) {
        str_deinit(&full);
        return luaL_error(L, "render: unsafe template path");
    }

    char real_root[PATH_MAX];
    char real_full[PATH_MAX];
    if (realpath(root, real_root) && realpath(full.data, real_full)) {
        size_t rl = strlen(real_root);
        bool inside = strncmp(real_full, real_root, rl) == 0 &&
                      (real_full[rl] == '/' || real_full[rl] == '\0');
        if (!inside) {
            str_deinit(&full);
            return luaL_error(L, "render: template escapes template_root");
        }
    }

    lua_getglobal(L, "tmg");
    if (!lua_istable(L, -1)) {
        str_deinit(&full);
        return luaL_error(L, "render: tmg module is not available");
    }
    lua_getfield(L, -1, "render_string");
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        str_deinit(&full);
        return luaL_error(L, "render: tmg.render_string is not available");
    }

    bool autoescape = true;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "autoescape");
        if (!lua_isnil(L, -1)) {
            autoescape = lua_toboolean(L, -1) != 0;
        } else {
            lua_pop(L, 1);
            lua_getfield(L, 4, "html");
            if (!lua_isnil(L, -1)) autoescape = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    } else if (lua_isboolean(L, 4)) {
        autoescape = lua_toboolean(L, 4) != 0;
    }

    lua_pushlstring(L, full.data, full.len);
    lua_pushnil(L); /* default delimiter */
    if (lua_isnoneornil(L, 3)) lua_pushnil(L);
    else lua_pushvalue(L, 3);
    lua_createtable(L, 0, 1);
    lua_pushboolean(L, autoescape);
    lua_setfield(L, -2, "autoescape");

    if (lua_pcall(L, 4, 2, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        str_deinit(&full);
        return luaL_error(L, "render: %s", err ? err : "failed");
    }
    str_deinit(&full);

    if (lua_isnil(L, -2)) {
        const char *err = lua_tostring(L, -1);
        return luaL_error(L, "render: %s", err ? err : "failed");
    }

    size_t len = 0;
    const char *body = lua_tolstring(L, -2, &len);
    if (!body) body = "";
    if (autoescape && !http_header_get(c->res_headers, c->nres_headers, "content-type"))
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Content-Type", "text/html; charset=utf-8", true);
    str_clear(&c->res_body);
    str_push_len(&c->res_body, body, len);
    c->ended = true;
    lua_settop(L, 1);
    return 1;
}

static int l_res_close(lua_State *L) {
    http_conn_t *c = check_conn(L);
    c->close_after_write = true;
    lua_settop(L, 1);
    return 1;
}

static const luaL_Reg res_methods[] = {
    {"status", l_res_status},
    {"header", l_res_header},
    {"send", l_res_send},
    {"json", l_res_json},
    {"html", l_res_html},
    {"redirect", l_res_redirect},
    {"cookie", l_res_cookie},
    {"render", l_res_render},
    {"close", l_res_close},
    {NULL, NULL}
};

static void ensure_res_metatable(lua_State *L) {
    if (luaL_newmetatable(L, HTTP_RES_MT)) {
        lua_newtable(L);
        for (const luaL_Reg *r = res_methods; r->name; r++) {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

int http_response_push(lua_State *L, http_conn_t *c) {
    ensure_res_metatable(L);
    http_res_t *r = (http_res_t *)lua_newuserdata(L, sizeof(http_res_t));
    r->conn = c;
    luaL_getmetatable(L, HTTP_RES_MT);
    lua_setmetatable(L, -2);
    return 1;
}

void http_response_reset(http_conn_t *c) {
    if (!c) return;
    http_headers_free(c->res_headers, c->nres_headers);
    c->res_headers = NULL;
    c->nres_headers = 0;
    c->cres_headers = 0;
    str_clear(&c->res_body);
    c->status = 200;
    c->ended = false;
    c->close_after_write = false;
}

bool http_conn_send_response(http_conn_t *c) {
    if (!c || c->closing) return false;
    if (c->writing) {
        http_log("response write already pending");
        http_conn_close(c);
        return false;
    }

    int status = c->status ? c->status : 200;
    bool head = c->method && strcmp(c->method, "HEAD") == 0;
    bool no_body = (status == 204 || status == 304);

    str_t out = str_init(512 + c->res_body.len);
    char line[128];
    snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", status,
             http_status_reason(status));
    str_push(&out, line);

    char date[64];
    http_date_now(date, sizeof(date));
    if (!http_header_get(c->res_headers, c->nres_headers, "date")) {
        str_push(&out, "Date: ");
        str_push(&out, date);
        str_push(&out, "\r\n");
    }
    if (!http_header_get(c->res_headers, c->nres_headers, "server")) {
        str_push(&out, "Server: " HTTP_SERVER_NAME "\r\n");
    }

    for (size_t i = 0; i < c->nres_headers; i++) {
        const char *n = c->res_headers[i].name;
        if (strcasecmp(n, "content-length") == 0 ||
            strcasecmp(n, "transfer-encoding") == 0 ||
            strcasecmp(n, "connection") == 0)
            continue;
        str_push(&out, n);
        str_push(&out, ": ");
        str_push(&out, c->res_headers[i].value);
        str_push(&out, "\r\n");
    }

    if (!no_body) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", c->res_body.len);
        str_push(&out, line);
    }

    bool close = c->close_after_write || !c->keep_alive;
    str_push(&out, close ? "Connection: close\r\n" : "Connection: keep-alive\r\n");
    str_push(&out, "\r\n");

    if (!no_body && !head)
        str_push_len(&out, c->res_body.data ? c->res_body.data : "",
                     c->res_body.len);

    c->write_buf = out.data;
    c->write_len = out.len;
    c->writing = true;
    c->write_req.data = c;

    uv_buf_t buf = uv_buf_init(c->write_buf, (unsigned int)c->write_len);
    int rc = uv_write(&c->write_req, (uv_stream_t *)&c->handle, &buf, 1,
                      http_conn_write_cb);
    if (rc != 0) {
        http_log("uv_write failed: %s", uv_strerror(rc));
        free(c->write_buf);
        c->write_buf = NULL;
        c->writing = false;
        http_conn_close(c);
        return false;
    }
    return true;
}

void http_conn_send_error(http_conn_t *c, int status, const char *msg) {
    if (!c || c->closing) return;
    if (c->reading) {
        uv_read_stop((uv_stream_t *)&c->handle);
        c->reading = false;
    }
    http_response_reset(c);
    c->status = status;
    c->keep_alive = false;
    c->close_after_write = true;

    const char *body = msg && *msg ? msg : http_status_reason(status);
    http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                     "Content-Type", "text/plain; charset=utf-8", true);
    str_clear(&c->res_body);
    str_push(&c->res_body, body);
    c->ended = true;
    http_conn_send_response(c);
}
