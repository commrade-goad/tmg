#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void http_oom(void) {
    fprintf(stderr, "ERR: http: out of memory\n");
    abort();
}

void *http_realloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n != 0)
        http_oom();
    return q;
}

char *http_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) http_oom();
    memcpy(p, s, n + 1);
    return p;
}

char *http_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = malloc(n + 1);
    if (!p) http_oom();
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int http_absindex(lua_State *L, int idx) {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX) return idx;
    return lua_gettop(L) + idx + 1;
}

void http_headers_free(http_header_t *h, size_t n) {
    if (!h) return;
    for (size_t i = 0; i < n; i++) {
        free(h[i].name);
        free(h[i].value);
    }
    free(h);
}

bool http_headers_add(http_header_t **arr, size_t *n, size_t *cap,
                      const char *name, const char *value, bool replace) {
    if (!arr || !n || !cap || !name || !value) return false;

    if (replace) {
        for (size_t i = 0; i < *n; i++) {
            if (strcasecmp((*arr)[i].name, name) == 0) {
                char *v = http_strdup(value);
                if (!v) return false;
                free((*arr)[i].value);
                (*arr)[i].value = v;
                return true;
            }
        }
    }

    if (*n >= *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        if (nc <= *cap) return false;
        http_header_t *p = http_realloc(*arr, sizeof(http_header_t) * nc);
        *arr = p;
        *cap = nc;
    }

    char *nn = http_strdup(name);
    char *nv = http_strdup(value);
    if (!nn || !nv) {
        free(nn);
        free(nv);
        return false;
    }
    (*arr)[*n].name = nn;
    (*arr)[*n].value = nv;
    (*n)++;
    return true;
}

const char *http_header_get(const http_header_t *h, size_t n, const char *name) {
    if (!h || !name) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(h[i].name, name) == 0)
            return h[i].value;
    }
    return NULL;
}

const char *http_status_reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Unknown";
    }
}

void http_date_now(char *buf, size_t n) {
    static const char *wday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t t = time(NULL);
    struct tm tm;
    if (!gmtime_r(&t, &tm)) {
        snprintf(buf, n, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }
    snprintf(buf, n, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             wday[tm.tm_wday], tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool http_percent_decode(const char *in, size_t len, bool plus, str_t *out) {
    if (!out) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%' && i + 2 < len) {
            int hi = hexval((unsigned char)in[i + 1]);
            int lo = hexval((unsigned char)in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                str_push_chr(out, (char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (plus && c == '+') {
            str_push_chr(out, ' ');
        } else {
            str_push_chr(out, (char)c);
        }
    }
    return true;
}

bool http_parse_urlencoded(lua_State *L, const char *data, size_t len,
                           int table_idx) {
    if (!L || !data) return false;
    table_idx = http_absindex(L, table_idx);

    size_t pos = 0;
    while (pos <= len) {
        size_t end = pos;
        while (end < len && data[end] != '&') end++;

        size_t eq = pos;
        while (eq < end && data[eq] != '=') eq++;

        str_t k = str_init(16);
        str_t v = str_init(16);
        http_percent_decode(data + pos, eq - pos, true, &k);
        if (eq < end)
            http_percent_decode(data + eq + 1, end - eq - 1, true, &v);

        if (k.len > 0) {
            lua_pushlstring(L, k.data, k.len);
            lua_pushlstring(L, v.data, v.len);
            lua_settable(L, table_idx);
        }

        str_deinit(&k);
        str_deinit(&v);

        if (end >= len) break;
        pos = end + 1;
    }
    return true;
}

bool http_path_safe(const char *root, const char *rel, str_t *out) {
    if (!root || !rel || !out) return false;
    if (rel[0] == '/') return false;

    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 2 && start[0] == '.' && start[1] == '.') return false;
        if (n == 1 && start[0] == '.') return false;
    }

    str_clear(out);
    str_push(out, root);
    size_t rl = strlen(root);
    if (rl == 0 || root[rl - 1] != '/')
        str_push_chr(out, '/');
    str_push(out, rel);
    return true;
}

char *http_sanitize_filename(const char *name) {
    if (!name) return http_strdup("upload");
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    if (!*base) base = "upload";

    char buf[160];
    size_t j = 0;
    for (const char *p = base; *p && j + 1 < sizeof(buf); p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '.' || c == '_' || c == '-')
            buf[j++] = (char)c;
        else
            buf[j++] = '_';
    }
    buf[j] = '\0';

    if (j == 0 || strcmp(buf, ".") == 0 || strcmp(buf, "..") == 0)
        return http_strdup("upload");
    return http_strdup(buf);
}

bool http_mkdir_p(const char *path) {
    if (!path || !*path) return false;
    char *tmp = http_strdup(path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return false;
    }
    free(tmp);
    return true;
}

void http_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "http: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
