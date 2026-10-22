#include "http.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

static void config_defaults(http_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->host = http_strdup("127.0.0.1");
    cfg->port = 8080;
    cfg->max_body = 10 * 1024 * 1024;
    cfg->max_header_bytes = 64 * 1024;
    cfg->max_headers = 100;
    cfg->max_url = 8192;
    cfg->max_static_size = 64 * 1024 * 1024;
    cfg->max_ws_message = 10 * 1024 * 1024;
    cfg->max_response = 10 * 1024 * 1024;
    cfg->header_timeout = 10;
    cfg->body_timeout = 15;
    cfg->handler_timeout = 30;
    cfg->write_timeout = 30;
    cfg->keepalive_timeout = 15;
    cfg->upload_dir = http_strdup("uploads");
    cfg->template_root = http_strdup(".");
    cfg->template_cache_mode = TMG_CACHE_DEVELOPMENT;
    cfg->template_cache_max = 256;
    cfg->max_multipart_parts = 64;
    cfg->max_multipart_files = 32;
    cfg->debug = false;
}

static void config_free(http_config_t *cfg) {
    if (!cfg) return;
    free(cfg->host);
    free(cfg->upload_dir);
    free(cfg->template_root);
    memset(cfg, 0, sizeof(*cfg));
}

static void state_defaults(http_state_t *st) {
    memset(st, 0, sizeof(*st));
    config_defaults(&st->cfg);
}

static bool read_config_integer(lua_State *L, int table, const char *name,
                                lua_Integer *value, bool *present) {
    lua_getfield(L, table, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        *present = false;
        return true;
    }
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_Number n = lua_tonumber(L, -1);
    lua_Integer v = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if ((lua_Number)v != n) return false;
    *value = v;
    *present = true;
    return true;
}

static bool read_size_config(lua_State *L, int table, const char *name,
                             size_t *target, char *err, size_t errsz) {
    lua_Integer v = 0;
    bool present = false;
    if (!read_config_integer(L, table, name, &v, &present)) {
        snprintf(err, errsz, "%s must be an integer", name);
        return false;
    }
    if (!present) return true;
    if (v < 0) {
        snprintf(err, errsz, "%s must be >= 0", name);
        return false;
    }
    if ((uintmax_t)v > (uintmax_t)SIZE_MAX) {
        snprintf(err, errsz, "%s is too large", name);
        return false;
    }
    *target = (size_t)v;
    return true;
}

static bool read_timeout_config(lua_State *L, int table, const char *name,
                                unsigned *target, char *err, size_t errsz) {
    lua_Integer v = 0;
    bool present = false;
    if (!read_config_integer(L, table, name, &v, &present)) {
        snprintf(err, errsz, "%s must be an integer", name);
        return false;
    }
    if (!present) return true;
    if (v < 0) {
        snprintf(err, errsz, "%s must be >= 0", name);
        return false;
    }
    if ((uintmax_t)v > (uintmax_t)UINT_MAX) {
        snprintf(err, errsz, "%s is too large", name);
        return false;
    }
    *target = (unsigned)v;
    return true;
}

static bool read_template_cache(lua_State *L, int table, int *mode,
                                char *err, size_t errsz) {
    lua_getfield(L, table, "template_cache");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (lua_isboolean(L, -1)) {
        *mode = lua_toboolean(L, -1) ? TMG_CACHE_DEVELOPMENT : TMG_CACHE_OFF;
        lua_pop(L, 1);
        return true;
    }
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        snprintf(err, errsz, "template_cache must be a string or boolean");
        return false;
    }
    const char *s = lua_tostring(L, -1);
    if (strcmp(s, "development") == 0 || strcmp(s, "dev") == 0)
        *mode = TMG_CACHE_DEVELOPMENT;
    else if (strcmp(s, "production") == 0 || strcmp(s, "prod") == 0)
        *mode = TMG_CACHE_PRODUCTION;
    else if (strcmp(s, "off") == 0 || strcmp(s, "none") == 0)
        *mode = TMG_CACHE_OFF;
    else {
        lua_pop(L, 1);
        snprintf(err, errsz,
                 "template_cache must be 'development', 'production', or false");
        return false;
    }
    lua_pop(L, 1);
    return true;
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
    if (st->deinitialized)
        return luaL_error(L, "http.listen: server is shut down");
    if (st->listening)
        return luaL_error(L, "http.listen: server is already running");

    if (!lua_isnoneornil(L, 1))
        luaL_checktype(L, 1, LUA_TTABLE);

    http_config_t cfg;
    config_defaults(&cfg);
    char err[160] = "";

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "host");
        if (!lua_isnil(L, -1)) {
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                snprintf(err, sizeof(err), "host must be a string");
                goto config_error;
            }
            free(cfg.host);
            cfg.host = http_strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        lua_Integer p = 0;
        bool has_port = false;
        if (!read_config_integer(L, 1, "port", &p, &has_port)) {
            snprintf(err, sizeof(err), "port must be an integer");
            goto config_error;
        }
        if (has_port) {
            if (p < 0 || p > 65535) {
                snprintf(err, sizeof(err), "invalid port");
                goto config_error;
            }
            cfg.port = (int)p;
        }

        if (!read_size_config(L, 1, "max_body", &cfg.max_body,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "max_header_bytes", &cfg.max_header_bytes,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "max_headers", &cfg.max_headers,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "max_url", &cfg.max_url,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "max_static_size", &cfg.max_static_size,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "max_ws_message", &cfg.max_ws_message,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "max_response", &cfg.max_response,
                              err, sizeof(err)) ||
            !read_size_config(L, 1, "template_cache_max",
                              &cfg.template_cache_max, err, sizeof(err)) ||
            !read_size_config(L, 1, "max_multipart_parts",
                              &cfg.max_multipart_parts, err, sizeof(err)) ||
            !read_size_config(L, 1, "max_multipart_files",
                              &cfg.max_multipart_files, err, sizeof(err)) ||
            !read_timeout_config(L, 1, "header_timeout", &cfg.header_timeout,
                                 err, sizeof(err)) ||
            !read_timeout_config(L, 1, "body_timeout", &cfg.body_timeout,
                                 err, sizeof(err)) ||
            !read_timeout_config(L, 1, "handler_timeout", &cfg.handler_timeout,
                                 err, sizeof(err)) ||
            !read_timeout_config(L, 1, "write_timeout", &cfg.write_timeout,
                                 err, sizeof(err)) ||
            !read_timeout_config(L, 1, "keepalive_timeout", &cfg.keepalive_timeout,
                                 err, sizeof(err)))
            goto config_error;

        lua_getfield(L, 1, "upload_dir");
        if (!lua_isnil(L, -1)) {
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                snprintf(err, sizeof(err), "upload_dir must be a string");
                goto config_error;
            }
            free(cfg.upload_dir);
            cfg.upload_dir = http_strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "template_root");
        if (!lua_isnil(L, -1)) {
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                snprintf(err, sizeof(err), "template_root must be a string");
                goto config_error;
            }
            free(cfg.template_root);
            cfg.template_root = http_strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        if (!read_template_cache(L, 1, &cfg.template_cache_mode,
                                 err, sizeof(err)))
            goto config_error;

        lua_getfield(L, 1, "debug");
        if (!lua_isnil(L, -1)) cfg.debug = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (cfg.max_header_bytes == 0) cfg.max_header_bytes = 64 * 1024;
    if (cfg.max_headers == 0) cfg.max_headers = 100;
    if (cfg.max_url == 0) cfg.max_url = 8192;
    if (cfg.max_response == 0) cfg.max_response = 10 * 1024 * 1024;
    if (cfg.template_cache_max == 0) cfg.template_cache_max = 256;
    if (cfg.max_multipart_parts == 0) cfg.max_multipart_parts = 64;
    if (cfg.max_multipart_files == 0) cfg.max_multipart_files = 32;

    if (!st->template_cache) {
        st->template_cache = tmg_template_cache_create(cfg.template_cache_mode,
                                                       cfg.template_cache_max);
        if (!st->template_cache) {
            snprintf(err, sizeof(err), "out of memory");
            goto config_error;
        }
    } else {
        tmg_template_cache_set(st->template_cache, L, cfg.template_cache_mode,
                               cfg.template_cache_max);
    }

    config_free(&st->cfg);
    st->cfg = cfg;

    int rc = http_state_start(L, st);
    if (rc != 0)
        return luaL_error(L, "http.listen: %s", uv_strerror(rc));

    lua_pushboolean(L, 1);
    return 1;

config_error:
    config_free(&cfg);
    return luaL_error(L, "http.listen: %s", err);
}

static int l_http_stop(lua_State *L) {
    http_state_t *st = ensure_state(L);
    http_state_stop(st);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_http_template_cache_clear(lua_State *L) {
    http_state_t *st = ensure_state(L);
    tmg_template_cache_clear(st->template_cache, L);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_http_template_cache_stats(lua_State *L) {
    http_state_t *st = ensure_state(L);
    return tmg_template_cache_stats(L, st->template_cache);
}

static int l_http_static(lua_State *L) {
    http_state_t *st = ensure_state(L);
    const char *prefix = luaL_checkstring(L, 1);
    const char *root = luaL_checkstring(L, 2);
    str_t err = str_init(64);
    if (!http_static_add(st, prefix, root, &err)) {
        const char *msg = err.len ? err.data : "invalid static mount";
        str_deinit(&err);
        return luaL_error(L, "http.static: %s", msg);
    }
    str_deinit(&err);
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
static int l_http_ws(lua_State *L) { return route_common(L, "WS"); }

static int l_http_route(lua_State *L) {
    char *method = http_strdup(luaL_checkstring(L, 1));
    lua_remove(L, 1);
    int rc = route_common(L, method);
    free(method);
    return rc;
}

static bool json_read_options(lua_State *L, int idx, size_t *max_output,
                              int *max_depth, char *err, size_t errsz) {
    *max_output = 0;
    *max_depth = HTTP_JSON_DEFAULT_DEPTH;
    if (lua_isnoneornil(L, idx)) return true;
    if (!lua_istable(L, idx)) {
        snprintf(err, errsz, "options must be a table");
        return false;
    }

    lua_Integer v = 0;
    bool present = false;
    if (!read_config_integer(L, idx, "max_output", &v, &present)) {
        snprintf(err, errsz, "max_output must be an integer");
        return false;
    }
    if (present) {
        if (v < 0) {
            snprintf(err, errsz, "max_output must be >= 0");
            return false;
        }
        *max_output = (size_t)v;
    }

    if (!read_config_integer(L, idx, "max_depth", &v, &present)) {
        snprintf(err, errsz, "max_depth must be an integer");
        return false;
    }
    if (present) {
        if (v < 1) {
            snprintf(err, errsz, "max_depth must be >= 1");
            return false;
        }
        if (v > HTTP_JSON_MAX_DEPTH) {
            snprintf(err, errsz, "max_depth is too large");
            return false;
        }
        *max_depth = (int)v;
    }
    return true;
}

static int l_http_json_encode(lua_State *L) {
    luaL_checkany(L, 1);
    size_t max_output = 0;
    int max_depth = HTTP_JSON_DEFAULT_DEPTH;
    char err[160] = "";
    if (!json_read_options(L, 2, &max_output, &max_depth, err, sizeof(err))) {
        lua_pushnil(L);
        lua_pushfstring(L, "http.json_encode: %s", err);
        return 2;
    }

    str_t out = str_init(64);
    if (!http_json_encode(L, 1, &out, max_output, max_depth)) {
        str_deinit(&out);
        lua_pushnil(L);
        lua_pushstring(L, "json: unsupported value, cycle, or too deeply nested");
        return 2;
    }
    lua_pushlstring(L, out.data, out.len);
    str_deinit(&out);
    return 1;
}

static int l_http_json_decode(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    size_t max_output = 0;
    int max_depth = HTTP_JSON_DEFAULT_DEPTH;
    char err[160] = "";
    if (!json_read_options(L, 2, &max_output, &max_depth, err, sizeof(err))) {
        lua_pushnil(L);
        lua_pushfstring(L, "http.json_decode: %s", err);
        return 2;
    }
    (void)max_output;

    int top = lua_gettop(L);
    str_t decode_err = str_init(64);
    if (!http_json_decode(L, s, len, max_depth, &decode_err)) {
        lua_settop(L, top);
        const char *msg = decode_err.len ? decode_err.data : "invalid JSON";
        lua_pushnil(L);
        lua_pushlstring(L, msg, decode_err.len ? decode_err.len : strlen(msg));
        str_deinit(&decode_err);
        return 2;
    }
    str_deinit(&decode_err);
    return 1;
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

    if (llhttp_get_upgrade(&c->parser)) {
        if (strcmp(method, "GET") != 0) {
            http_conn_send_error(c, 400, "WebSocket upgrade requires GET");
            str_deinit(&path);
            free(raw_path);
            return 0;
        }
        http_route_t *wsroute = http_route_match(st, "WS", path.data);
        if (!wsroute) {
            http_conn_send_error(c, 404, http_status_reason(404));
            str_deinit(&path);
            free(raw_path);
            return 0;
        }
        if (!http_build_request(L, c, path.data, query, raw_path, wsroute)) {
            http_conn_send_error(c, 400, http_status_reason(400));
            str_deinit(&path);
            free(raw_path);
            return 0;
        }

        int base = lua_gettop(L); /* request table */
        bool ok = http_ws_init(c, L, wsroute, base);
        lua_settop(L, base - 1);
        str_deinit(&path);
        free(raw_path);
        return ok ? 1 : 0;
    }

    http_route_t *route = http_route_match(st, method, path.data);
    if (!route && strcmp(method, "HEAD") == 0)
        route = http_route_match(st, "GET", path.data);

    if (!route) {
        if (http_static_serve(c, method, path.data)) {
            str_deinit(&path);
            free(raw_path);
            return 0;
        }
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
        http_response_detach_all(c);
        const char *body = st->cfg.debug && emsg.len ? emsg.data : "Internal Server Error";
        http_conn_send_error(c, 500, body);
        str_deinit(&emsg);
    } else {
        lua_settop(L, base - 1);
        if (!c->ended) c->ended = true;
        http_response_detach_all(c);
        http_conn_send_response(c);
    }

    str_deinit(&path);
    free(raw_path);
    return rc == LUA_OK;
}

static const luaL_Reg http_lib[] = {
    {"listen", l_http_listen},
    {"stop", l_http_stop},
    {"template_cache_clear", l_http_template_cache_clear},
    {"template_cache_stats", l_http_template_cache_stats},
    {"static", l_http_static},
    {"route", l_http_route},
    {"json_encode", l_http_json_encode},
    {"json_decode", l_http_json_decode},
    {"get", l_http_get},
    {"post", l_http_post},
    {"put", l_http_put},
    {"patch", l_http_patch},
    {"delete", l_http_delete},
    {"head", l_http_head},
    {"options", l_http_options},
    {"ws", l_http_ws},
    {NULL, NULL}
};

int luaopen_http(lua_State *L) {
    http_state_t *st = ensure_state(L);
    st->L = L;
    luaL_newlib(L, http_lib);
    return 1;
}
