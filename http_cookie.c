#include "http.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int absindex(lua_State *L, int idx) {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX) return idx;
    return lua_gettop(L) + idx + 1;
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

static bool valid_name(const char *s) {
    if (!s || !*s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!token_char(*p)) return false;
    return true;
}

static bool valid_value(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == 0x21) continue;
        if (c >= 0x23 && c <= 0x2B) continue;
        if (c >= 0x2D && c <= 0x3A) continue;
        if (c >= 0x3C && c <= 0x5B) continue;
        if (c >= 0x5D && c <= 0x7E) continue;
        return false;
    }
    return true;
}

static bool valid_attr(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == ';')
            return false;
    }
    return true;
}

bool http_cookies_parse(lua_State *L, const char *cookie, int table_idx) {
    if (!L || !cookie) return false;
    table_idx = absindex(L, table_idx);

    const char *p = cookie;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && *p != ';') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;

        const char *eq = start;
        while (eq < end && *eq != '=') eq++;
        if (eq < end) {
            const char *ns = start;
            const char *ne = eq;
            while (ne > ns && isspace((unsigned char)ne[-1])) ne--;
            const char *vs = eq + 1;
            while (vs < end && isspace((unsigned char)*vs)) vs++;
            const char *ve = end;
            if (ve > vs + 1 && *vs == '"' && ve[-1] == '"') {
                vs++;
                ve--;
            }
            if (ne > ns) {
                lua_pushlstring(L, ns, (size_t)(ne - ns));
                lua_pushlstring(L, vs, (size_t)(ve - vs));
                lua_settable(L, table_idx);
            }
        }
        if (*p == ';') p++;
    }
    return true;
}

bool http_cookie_header(http_conn_t *c, const char *name, const char *value,
                        int opts_idx, lua_State *L) {
    if (!c || !L) return false;
    if (!valid_name(name))
        luaL_error(L, "cookie: invalid name");
    if (!value) value = "";
    if (!valid_value(value))
        luaL_error(L, "cookie: value contains unsafe characters");

    opts_idx = opts_idx ? absindex(L, opts_idx) : 0;

    bool httponly = true;
    bool secure = false;
    const char *path = "/";
    const char *domain = NULL;
    const char *expires = NULL;
    const char *samesite = "Lax";
    int64_t max_age = 0;
    bool has_max_age = false;

    if (opts_idx && lua_istable(L, opts_idx)) {
        lua_getfield(L, opts_idx, "httponly");
        if (!lua_isnil(L, -1)) httponly = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "secure");
        secure = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "path");
        if (lua_isstring(L, -1)) path = lua_tostring(L, -1);
        else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) path = NULL;
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "domain");
        if (lua_isstring(L, -1)) domain = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "expires");
        if (lua_isstring(L, -1)) expires = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "max_age");
        if (lua_isnumber(L, -1)) {
            max_age = (int64_t)lua_tointeger(L, -1);
            has_max_age = true;
        }
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "samesite");
        if (lua_isstring(L, -1)) {
            samesite = lua_tostring(L, -1);
        } else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            samesite = NULL;
        }
        lua_pop(L, 1);
    }

    if (path && !valid_attr(path))
        luaL_error(L, "cookie: invalid path");
    if (domain && !valid_attr(domain))
        luaL_error(L, "cookie: invalid domain");
    if (expires && !valid_attr(expires))
        luaL_error(L, "cookie: invalid expires");

    const char *ss = NULL;
    if (samesite && *samesite) {
        if (strcasecmp(samesite, "strict") == 0) ss = "Strict";
        else if (strcasecmp(samesite, "lax") == 0) ss = "Lax";
        else if (strcasecmp(samesite, "none") == 0) ss = "None";
        else luaL_error(L, "cookie: samesite must be Strict, Lax or None");
    }

    str_t out = str_init(128);
    str_push(&out, name);
    str_push_chr(&out, '=');
    str_push(&out, value);

    if (path && *path) {
        str_push(&out, "; Path=");
        str_push(&out, path);
    }
    if (domain && *domain) {
        str_push(&out, "; Domain=");
        str_push(&out, domain);
    }
    if (has_max_age) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "; Max-Age=%lld", (long long)max_age);
        str_push(&out, tmp);
    }
    if (expires && *expires) {
        str_push(&out, "; Expires=");
        str_push(&out, expires);
    }
    if (httponly) str_push(&out, "; HttpOnly");
    if (secure) str_push(&out, "; Secure");
    if (ss) {
        str_push(&out, "; SameSite=");
        str_push(&out, ss);
    }

    bool ok = http_headers_add(&c->res_headers, &c->nres_headers,
                               &c->cres_headers, "Set-Cookie", out.data, false);
    str_deinit(&out);
    return ok;
}
