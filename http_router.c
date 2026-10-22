#include "http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool next_seg(const char **pp, const char **seg, size_t *len) {
    const char *p = *pp;
    while (*p == '/') p++;
    if (!*p) {
        *pp = p;
        return false;
    }
    *seg = p;
    while (*p && *p != '/') p++;
    *len = (size_t)(p - *seg);
    *pp = p;
    return true;
}

static char *upper_dup(const char *s) {
    char *p = http_strdup(s);
    if (!p) return NULL;
    for (char *q = p; *q; q++) *q = (char)toupper((unsigned char)*q);
    return p;
}

bool http_route_add(http_state_t *st, const char *method, const char *pattern,
                    int handler_ref, str_t *err) {
    if (!st || !method || !pattern) return false;
    if (pattern[0] != '/') {
        if (err) { str_clear(err); str_push(err, "route pattern must start with '/'"); }
        return false;
    }

    size_t nsegs = 0;
    for (const char *p = pattern; *p; p++) {
        while (*p == '/') p++;
        if (!*p) break;
        nsegs++;
        while (*p && *p != '/') p++;
        if (!*p) break;
    }

    char **segs = calloc(nsegs ? nsegs : 1, sizeof(char *));
    if (!segs) return false;

    size_t idx = 0;
    bool dynamic = false;
    const char *p = pattern;
    const char *seg;
    size_t len;
    while (next_seg(&p, &seg, &len)) {
        if (len == 0) continue;
        if (idx >= nsegs) break;
        if (seg[0] == ':' || seg[0] == '*') {
            if (len < 2) {
                if (err) { str_clear(err); str_push(err, "empty route parameter name"); }
                goto fail;
            }
            dynamic = true;
            if (seg[0] == '*') {
                /* wildcard must be the final segment */
                const char *q = seg + len;
                while (*q == '/') q++;
                if (*q) {
                    if (err) { str_clear(err); str_push(err, "'*' must be the last route segment"); }
                    goto fail;
                }
            }
        }
        segs[idx] = http_strndup(seg, len);
        if (!segs[idx]) goto fail;
        idx++;
    }

    if (st->nroutes >= st->croutes) {
        size_t nc = st->croutes ? st->croutes * 2 : 8;
        http_route_t *r = http_realloc(st->routes, sizeof(http_route_t) * nc);
        st->routes = r;
        st->croutes = nc;
    }

    http_route_t *r = &st->routes[st->nroutes++];
    r->method = upper_dup(method);
    r->pattern = http_strdup(pattern);
    r->segs = segs;
    r->nsegs = idx;
    r->dynamic = dynamic;
    r->handler_ref = handler_ref;
    if (!r->method || !r->pattern) {
        st->nroutes--;
        free(r->method);
        free(r->pattern);
        for (size_t i = 0; i < idx; i++) free(segs[i]);
        free(segs);
        return false;
    }
    return true;

fail:
    for (size_t i = 0; i < idx; i++) free(segs[i]);
    free(segs);
    return false;
}

bool http_route_match_path(http_route_t *r, const char *path) {
    if (!r || !path) return false;
    const char *p = path;
    const char *seg;
    size_t len;

    for (size_t i = 0; i < r->nsegs; i++) {
        bool have = next_seg(&p, &seg, &len);
        const char *ps = r->segs[i];
        if (ps[0] == ':') {
            if (!have || len == 0) return false;
        } else if (ps[0] == '*') {
            return true;
        } else {
            size_t pl = strlen(ps);
            if (!have || len != pl || memcmp(seg, ps, pl) != 0)
                return false;
        }
    }

    while (*p == '/') p++;
    return *p == '\0';
}

http_route_t *http_route_match(http_state_t *st, const char *method,
                               const char *path) {
    if (!st || !method || !path) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < st->nroutes; i++) {
            http_route_t *r = &st->routes[i];
            if (r->dynamic != (pass == 1)) continue;
            if (strcmp(r->method, method) != 0) continue;
            if (http_route_match_path(r, path))
                return r;
        }
    }
    return NULL;
}

void http_route_fill_params(http_route_t *r, const char *path, lua_State *L,
                            int table_idx) {
    if (!r || !L) return;
    table_idx = table_idx < 0 ? lua_gettop(L) + table_idx + 1 : table_idx;

    const char *p = path;
    const char *seg;
    size_t len;
    for (size_t i = 0; i < r->nsegs; i++) {
        bool have = next_seg(&p, &seg, &len);
        const char *ps = r->segs[i];
        if (ps[0] == ':') {
            if (!have) return;
            lua_pushlstring(L, ps + 1, strlen(ps + 1));
            lua_pushlstring(L, seg, len);
            lua_settable(L, table_idx);
        } else if (ps[0] == '*') {
            while (*p == '/') p++;
            lua_pushlstring(L, ps + 1, strlen(ps + 1));
            lua_pushstring(L, p);
            lua_settable(L, table_idx);
            return;
        }
    }
}

static bool allow_has(const str_t *allow, const char *method) {
    size_t mlen = strlen(method);
    const char *p = allow->data;
    if (!p) return false;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - s);
        while (n > 0 && (s[n - 1] == ' ')) n--;
        if (n == mlen && memcmp(s, method, mlen) == 0)
            return true;
    }
    return false;
}

bool http_route_allowed(http_state_t *st, const char *path, str_t *allow) {
    if (!st || !path || !allow) return false;
    bool any = false;
    for (size_t i = 0; i < st->nroutes; i++) {
        http_route_t *r = &st->routes[i];
        if (!http_route_match_path(r, path)) continue;
        if (allow_has(allow, r->method)) continue;
        if (any) str_push(allow, ", ");
        str_push(allow, r->method);
        any = true;
    }
    return any;
}

void http_routes_free(http_route_t *routes, size_t n) {
    if (!routes) return;
    for (size_t i = 0; i < n; i++) {
        free(routes[i].method);
        free(routes[i].pattern);
        for (size_t j = 0; j < routes[i].nsegs; j++)
            free(routes[i].segs[j]);
        free(routes[i].segs);
    }
    free(routes);
}
