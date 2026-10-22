#include "http.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void json_escape(str_t *out, const char *s, size_t len) {
    str_push_chr(out, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': str_push(out, "\\\""); break;
        case '\\': str_push(out, "\\\\"); break;
        case '\b': str_push(out, "\\b"); break;
        case '\f': str_push(out, "\\f"); break;
        case '\n': str_push(out, "\\n"); break;
        case '\r': str_push(out, "\\r"); break;
        case '\t': str_push(out, "\\t"); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                str_push(out, tmp);
            } else {
                str_push_chr(out, (char)c);
            }
        }
    }
    str_push_chr(out, '"');
}

bool http_json_encode(lua_State *L, int idx, str_t *out, int depth) {
    if (!L || !out) return false;
    if (depth > 64) return false;

    idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
    int t = lua_type(L, idx);

    switch (t) {
    case LUA_TNIL:
        str_push(out, "null");
        return true;
    case LUA_TBOOLEAN:
        str_push(out, lua_toboolean(L, idx) ? "true" : "false");
        return true;
    case LUA_TNUMBER: {
        double d = lua_tonumber(L, idx);
        if (!isfinite(d)) {
            str_push(out, "null");
            return true;
        }
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.17g", d);
        str_push(out, tmp);
        return true;
    }
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        json_escape(out, s, len);
        return true;
    }
    case LUA_TTABLE: {
        size_t n = lua_objlen(L, idx);
        if (n > 0) {
            str_push_chr(out, '[');
            for (size_t i = 1; i <= n; i++) {
                if (i > 1) str_push_chr(out, ',');
                lua_rawgeti(L, idx, (int)i);
                if (!http_json_encode(L, -1, out, depth + 1))
                    str_push(out, "null");
                lua_pop(L, 1);
            }
            str_push_chr(out, ']');
            return true;
        }

        str_push_chr(out, '{');
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (!first) str_push_chr(out, ',');
            first = false;

            lua_pushvalue(L, -2);
            size_t klen = 0;
            const char *k = lua_tolstring(L, -1, &klen);
            if (k) json_escape(out, k, klen);
            else str_push(out, "\"\"");
            lua_pop(L, 1);

            str_push_chr(out, ':');
            if (!http_json_encode(L, -1, out, depth + 1))
                str_push(out, "null");
            lua_pop(L, 1);
        }
        str_push_chr(out, '}');
        return true;
    }
    default:
        str_push(out, "null");
        return true;
    }
}
