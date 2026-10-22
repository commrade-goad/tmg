#include "http.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool json_budget_ok(str_t *out, size_t max_output) {
    return max_output == 0 || out->len <= max_output;
}

static bool json_can_add(str_t *out, size_t extra, size_t max_output) {
    if (max_output == 0) return true;
    if (out->len > max_output) return false;
    return extra <= max_output - out->len;
}

typedef struct {
    str_t *out;
    size_t max_output;
    int max_depth;
    const void **stack;
    int stack_len;
} json_encoder_t;

static bool json_encode_value(lua_State *L, int idx, json_encoder_t *enc,
                              int depth) {
    if (!L || !enc || !enc->out) return false;
    if (depth > enc->max_depth) return false;
    if (!json_budget_ok(enc->out, enc->max_output)) return false;

    idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
    int t = lua_type(L, idx);

    switch (t) {
    case LUA_TNIL:
        if (!json_can_add(enc->out, 4, enc->max_output)) return false;
        str_push(enc->out, "null");
        return true;
    case LUA_TBOOLEAN:
        if (!json_can_add(enc->out, 5, enc->max_output)) return false;
        str_push(enc->out, lua_toboolean(L, idx) ? "true" : "false");
        return true;
    case LUA_TNUMBER: {
        double d = lua_tonumber(L, idx);
        if (!isfinite(d)) {
            if (!json_can_add(enc->out, 4, enc->max_output)) return false;
            str_push(enc->out, "null");
            return true;
        }
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.17g", d);
        if (!json_can_add(enc->out, strlen(tmp), enc->max_output)) return false;
        str_push(enc->out, tmp);
        return true;
    }
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        if (!json_can_add(enc->out, len, enc->max_output)) return false;
        json_escape(enc->out, s, len);
        return json_budget_ok(enc->out, enc->max_output);
    }
    case LUA_TTABLE: {
        const void *ptr = lua_topointer(L, idx);
        for (int i = 0; i < enc->stack_len; i++) {
            if (enc->stack[i] == ptr) return false;
        }
        if (enc->stack_len >= enc->max_depth + 1) return false;

        enc->stack[enc->stack_len++] = ptr;
        bool ok = false;
        size_t n = lua_objlen(L, idx);
        if (n > 0) {
            if (!json_can_add(enc->out, 2, enc->max_output)) goto done;
            str_push_chr(enc->out, '[');
            for (size_t i = 1; i <= n; i++) {
                if (i > 1) {
                    if (!json_can_add(enc->out, 1, enc->max_output)) goto done;
                    str_push_chr(enc->out, ',');
                }
                lua_rawgeti(L, idx, (int)i);
                if (!json_encode_value(L, -1, enc, depth + 1)) {
                    lua_pop(L, 1);
                    goto done;
                }
                lua_pop(L, 1);
                if (!json_budget_ok(enc->out, enc->max_output)) goto done;
            }
            if (!json_can_add(enc->out, 1, enc->max_output)) goto done;
            str_push_chr(enc->out, ']');
            ok = true;
            goto done;
        }

        if (!json_can_add(enc->out, 2, enc->max_output)) goto done;
        str_push_chr(enc->out, '{');
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (!first) {
                if (!json_can_add(enc->out, 1, enc->max_output)) {
                    lua_pop(L, 2);
                    goto done;
                }
                str_push_chr(enc->out, ',');
            }
            first = false;

            lua_pushvalue(L, -2);
            size_t klen = 0;
            const char *k = lua_tolstring(L, -1, &klen);
            if (k) {
                if (!json_can_add(enc->out, klen, enc->max_output)) {
                    lua_pop(L, 3);
                    goto done;
                }
                json_escape(enc->out, k, klen);
            } else {
                if (!json_can_add(enc->out, 2, enc->max_output)) {
                    lua_pop(L, 3);
                    goto done;
                }
                str_push(enc->out, "\"\"");
            }
            lua_pop(L, 1);
            if (!json_budget_ok(enc->out, enc->max_output)) {
                lua_pop(L, 2);
                goto done;
            }

            if (!json_can_add(enc->out, 1, enc->max_output)) {
                lua_pop(L, 2);
                goto done;
            }
            str_push_chr(enc->out, ':');
            if (!json_encode_value(L, -1, enc, depth + 1)) {
                lua_pop(L, 2);
                goto done;
            }
            lua_pop(L, 1);
            if (!json_budget_ok(enc->out, enc->max_output)) {
                lua_pop(L, 1);
                goto done;
            }
        }
        if (!json_can_add(enc->out, 1, enc->max_output)) goto done;
        str_push_chr(enc->out, '}');
        ok = true;
    done:
        enc->stack_len--;
        return ok;
    }
    default:
        if (!json_can_add(enc->out, 4, enc->max_output)) return false;
        str_push(enc->out, "null");
        return true;
    }
}

bool http_json_encode(lua_State *L, int idx, str_t *out, size_t max_output,
                      int max_depth) {
    if (!L || !out) return false;
    if (max_depth <= 0) max_depth = HTTP_JSON_DEFAULT_DEPTH;
    if (max_depth > HTTP_JSON_MAX_DEPTH) max_depth = HTTP_JSON_MAX_DEPTH;

    const void **stack = calloc((size_t)max_depth + 1, sizeof(*stack));
    if (!stack) return false;
    json_encoder_t enc = {
        .out = out,
        .max_output = max_output,
        .max_depth = max_depth,
        .stack = stack,
        .stack_len = 0,
    };
    bool ok = json_encode_value(L, idx, &enc, 0);
    free(stack);
    return ok;
}

typedef struct {
    const char *start;
    const char *p;
    const char *end;
    int depth;
    int max_depth;
    str_t *err;
} json_parser_t;

static bool json_error(json_parser_t *p, const char *msg) {
    if (p->err && p->err->len == 0) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "json: %s at byte %zu", msg,
                 (size_t)(p->p - p->start));
        str_push(p->err, tmp);
    }
    return false;
}

static void json_skip_ws(json_parser_t *p) {
    while (p->p < p->end) {
        char c = *p->p;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->p++;
    }
}

static bool json_hex4(json_parser_t *p, unsigned *out) {
    if ((size_t)(p->end - p->p) < 4)
        return json_error(p, "truncated unicode escape");
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *p->p++;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return json_error(p, "invalid unicode escape");
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static void json_utf8(str_t *out, unsigned cp) {
    if (cp <= 0x7f) {
        str_push_chr(out, (char)cp);
    } else if (cp <= 0x7ff) {
        str_push_chr(out, (char)(0xc0 | (cp >> 6)));
        str_push_chr(out, (char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        str_push_chr(out, (char)(0xe0 | (cp >> 12)));
        str_push_chr(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        str_push_chr(out, (char)(0x80 | (cp & 0x3f)));
    } else {
        str_push_chr(out, (char)(0xf0 | (cp >> 18)));
        str_push_chr(out, (char)(0x80 | ((cp >> 12) & 0x3f)));
        str_push_chr(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        str_push_chr(out, (char)(0x80 | (cp & 0x3f)));
    }
}

static bool json_parse_string(lua_State *L, json_parser_t *p) {
    if (p->p >= p->end || *p->p != '"')
        return json_error(p, "expected string");
    p->p++;

    str_t out = str_init(32);
    bool ok = false;
    while (p->p < p->end) {
        unsigned char c = (unsigned char)*p->p++;
        if (c == '"') {
            lua_pushlstring(L, out.data, out.len);
            ok = true;
            break;
        }
        if (c < 0x20) {
            json_error(p, "control character in string");
            break;
        }
        if (c != '\\') {
            str_push_chr(&out, (char)c);
            continue;
        }
        if (p->p >= p->end) {
            json_error(p, "truncated escape");
            break;
        }
        char e = *p->p++;
        switch (e) {
        case '"': str_push_chr(&out, '"'); break;
        case '\\': str_push_chr(&out, '\\'); break;
        case '/': str_push_chr(&out, '/'); break;
        case 'b': str_push_chr(&out, '\b'); break;
        case 'f': str_push_chr(&out, '\f'); break;
        case 'n': str_push_chr(&out, '\n'); break;
        case 'r': str_push_chr(&out, '\r'); break;
        case 't': str_push_chr(&out, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!json_hex4(p, &cp)) goto done;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if ((size_t)(p->end - p->p) < 6 || p->p[0] != '\\' ||
                    p->p[1] != 'u') {
                    json_error(p, "missing low surrogate");
                    goto done;
                }
                p->p += 2;
                unsigned low;
                if (!json_hex4(p, &low)) goto done;
                if (low < 0xdc00 || low > 0xdfff) {
                    json_error(p, "invalid low surrogate");
                    goto done;
                }
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                json_error(p, "unexpected low surrogate");
                goto done;
            }
            json_utf8(&out, cp);
            break;
        }
        default:
            json_error(p, "invalid escape");
            goto done;
        }
    }
    if (!ok && p->p >= p->end && (!p->err || p->err->len == 0))
        json_error(p, "unterminated string");
done:
    str_deinit(&out);
    return ok;
}

static bool json_parse_number(lua_State *L, json_parser_t *p) {
    const char *start = p->p;
    if (p->p < p->end && *p->p == '-') p->p++;
    if (p->p >= p->end) return json_error(p, "invalid number");
    if (*p->p == '0') {
        p->p++;
    } else if (*p->p >= '1' && *p->p <= '9') {
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    } else {
        return json_error(p, "invalid number");
    }
    if (p->p < p->end && *p->p == '.') {
        p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9')
            return json_error(p, "invalid fraction");
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
        p->p++;
        if (p->p < p->end && (*p->p == '+' || *p->p == '-')) p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9')
            return json_error(p, "invalid exponent");
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }

    size_t len = (size_t)(p->p - start);
    char *tmp = malloc(len + 1);
    if (!tmp) return json_error(p, "out of memory");
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    errno = 0;
    char *end = NULL;
    double d = strtod(tmp, &end);
    bool complete = end == tmp + len;
    bool range = errno == ERANGE;
    free(tmp);
    if (!complete || range || !isfinite(d))
        return json_error(p, "number out of range");
    lua_pushnumber(L, d);
    return true;
}

static bool json_parse_value(lua_State *L, json_parser_t *p);

static bool json_parse_array(lua_State *L, json_parser_t *p) {
    if (p->depth >= p->max_depth)
        return json_error(p, "maximum depth exceeded");
    p->depth++;
    p->p++;
    json_skip_ws(p);
    lua_newtable(L);
    if (p->p < p->end && *p->p == ']') {
        p->p++;
        p->depth--;
        return true;
    }

    int idx = 1;
    for (;;) {
        if (!json_parse_value(L, p)) return false;
        if (idx == INT_MAX) return json_error(p, "array too long");
        lua_rawseti(L, -2, idx++);
        json_skip_ws(p);
        if (p->p >= p->end) return json_error(p, "unterminated array");
        if (*p->p == ',') {
            p->p++;
            json_skip_ws(p);
            if (p->p < p->end && *p->p == ']')
                return json_error(p, "trailing comma in array");
            continue;
        }
        if (*p->p == ']') {
            p->p++;
            p->depth--;
            return true;
        }
        return json_error(p, "expected comma or closing bracket");
    }
}

static bool json_parse_object(lua_State *L, json_parser_t *p) {
    if (p->depth >= p->max_depth)
        return json_error(p, "maximum depth exceeded");
    p->depth++;
    p->p++;
    json_skip_ws(p);
    lua_newtable(L);
    if (p->p < p->end && *p->p == '}') {
        p->p++;
        p->depth--;
        return true;
    }

    for (;;) {
        json_skip_ws(p);
        if (!json_parse_string(L, p)) return false;
        json_skip_ws(p);
        if (p->p >= p->end || *p->p != ':')
            return json_error(p, "expected colon");
        p->p++;
        json_skip_ws(p);
        if (!json_parse_value(L, p)) return false;
        lua_settable(L, -3);
        json_skip_ws(p);
        if (p->p >= p->end) return json_error(p, "unterminated object");
        if (*p->p == ',') {
            p->p++;
            json_skip_ws(p);
            if (p->p < p->end && *p->p == '}')
                return json_error(p, "trailing comma in object");
            continue;
        }
        if (*p->p == '}') {
            p->p++;
            p->depth--;
            return true;
        }
        return json_error(p, "expected comma or closing brace");
    }
}

static bool json_parse_value(lua_State *L, json_parser_t *p) {
    json_skip_ws(p);
    if (p->p >= p->end) return json_error(p, "unexpected end of input");

    char c = *p->p;
    if (c == '"') return json_parse_string(L, p);
    if (c == '{') return json_parse_object(L, p);
    if (c == '[') return json_parse_array(L, p);
    if (c == 't') {
        if ((size_t)(p->end - p->p) >= 4 && memcmp(p->p, "true", 4) == 0) {
            p->p += 4;
            lua_pushboolean(L, 1);
            return true;
        }
        return json_error(p, "invalid literal");
    }
    if (c == 'f') {
        if ((size_t)(p->end - p->p) >= 5 && memcmp(p->p, "false", 5) == 0) {
            p->p += 5;
            lua_pushboolean(L, 0);
            return true;
        }
        return json_error(p, "invalid literal");
    }
    if (c == 'n') {
        if ((size_t)(p->end - p->p) >= 4 && memcmp(p->p, "null", 4) == 0) {
            p->p += 4;
            lua_pushnil(L);
            return true;
        }
        return json_error(p, "invalid literal");
    }
    if (c == '-' || (c >= '0' && c <= '9'))
        return json_parse_number(L, p);
    return json_error(p, "unexpected character");
}

bool http_json_decode(lua_State *L, const char *s, size_t len, int max_depth,
                      str_t *err) {
    if (!L || !s) return false;
    if (max_depth <= 0) max_depth = HTTP_JSON_DEFAULT_DEPTH;
    if (max_depth > HTTP_JSON_MAX_DEPTH) max_depth = HTTP_JSON_MAX_DEPTH;

    json_parser_t p = {
        .start = s,
        .p = s,
        .end = s + len,
        .depth = 0,
        .max_depth = max_depth,
        .err = err,
    };
    json_skip_ws(&p);
    if (p.p == p.end) return json_error(&p, "empty input");
    if (!json_parse_value(L, &p)) return false;
    json_skip_ws(&p);
    if (p.p != p.end) return json_error(&p, "trailing data");
    return true;
}
