#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int absindex(lua_State *L, int idx) {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX) return idx;
    return lua_gettop(L) + idx + 1;
}

static size_t find_bytes(const char *hay, size_t hlen, const char *needle,
                         size_t nlen, size_t start) {
    if (!hay || !needle || nlen == 0 || hlen < nlen) return SIZE_MAX;
    for (size_t i = start; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return i;
    }
    return SIZE_MAX;
}

static bool starts_ci(const char *s, size_t len, const char *word) {
    size_t n = strlen(word);
    if (len < n) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)word[i]))
            return false;
    }
    return true;
}

static bool get_boundary(const char *ct, str_t *out) {
    if (!ct) return false;
    size_t len = strlen(ct);
    for (size_t i = 0; i + 8 <= len; i++) {
        if (i > 0 && isalnum((unsigned char)ct[i - 1])) continue;
        if (!starts_ci(ct + i, len - i, "boundary")) continue;
        size_t j = i + 8;
        while (j < len && isspace((unsigned char)ct[j])) j++;
        if (j >= len || ct[j] != '=') continue;
        j++;
        while (j < len && isspace((unsigned char)ct[j])) j++;
        if (j >= len) return false;

        if (ct[j] == '"') {
            j++;
            size_t start = j;
            while (j < len && ct[j] != '"') j++;
            if (j >= len) return false;
            str_clear(out);
            str_push_len(out, ct + start, j - start);
        } else {
            size_t start = j;
            while (j < len && ct[j] != ';' && !isspace((unsigned char)ct[j])) j++;
            str_clear(out);
            str_push_len(out, ct + start, j - start);
        }
        if (out->len == 0 || out->len > 200) return false;
        for (size_t k = 0; k < out->len; k++) {
            if (out->data[k] == '\r' || out->data[k] == '\n')
                return false;
        }
        return true;
    }
    return false;
}

static bool find_param(const char *s, const char *param, str_t *out) {
    size_t len = strlen(s);
    size_t plen = strlen(param);
    for (size_t i = 0; i + plen <= len; i++) {
        if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))
            continue;
        if (!starts_ci(s + i, len - i, param)) continue;
        size_t j = i + plen;
        while (j < len && isspace((unsigned char)s[j])) j++;
        if (j >= len || s[j] != '=') continue;
        j++;
        while (j < len && isspace((unsigned char)s[j])) j++;

        str_clear(out);
        if (j < len && s[j] == '"') {
            j++;
            while (j < len && s[j] != '"') {
                if (s[j] == '\\' && j + 1 < len) j++;
                str_push_chr(out, s[j++]);
            }
            return true;
        }
        while (j < len && s[j] != ';' && !isspace((unsigned char)s[j]))
            str_push_chr(out, s[j++]);
        return true;
    }
    return false;
}

static char *trim_dup(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return http_strndup(s, n);
}

static bool parse_part_headers(const char *headers, size_t hlen, char **field,
                               char **filename, char **ctype) {
    *field = NULL;
    *filename = NULL;
    *ctype = NULL;

    char *copy = http_strndup(headers, hlen);
    if (!copy) return false;

    bool headers_ok = true;
    char *line = copy;
    while (line && *line) {
        char *next = strstr(line, "\r\n");
        if (next) {
            *next = '\0';
            next += 2;
        } else {
            next = strchr(line, '\n');
            if (next) {
                *next = '\0';
                next++;
            }
        }
        char *colon = strchr(line, ':');
        if (colon && colon != line) {
            *colon = '\0';
            char *name = trim_dup(line);
            char *value = trim_dup(colon + 1);
            if (name && value) {
                if (strcasecmp(name, "content-disposition") == 0) {
                    str_t t = str_init(32);
                    if (find_param(value, "name", &t))
                        *field = http_strdup(t.data);
                    if (find_param(value, "filename", &t))
                        *filename = http_strdup(t.data);
                    str_deinit(&t);
                } else if (strcasecmp(name, "content-type") == 0) {
                    *ctype = http_strdup(value);
                }
            }
            free(name);
            free(value);
        } else {
            headers_ok = false;
        }
        line = next;
    }

    free(copy);
    return headers_ok && *field != NULL;
}

static uint64_t upload_counter = 0;

static char *make_upload_path(http_conn_t *c, const char *filename) {
    char *safe = http_sanitize_filename(filename);
    if (!safe) return NULL;
    const char *dir = c->state->cfg.upload_dir;
    if (!dir || !*dir) dir = "uploads";
    if (!http_mkdir_p(dir)) {
        free(safe);
        return NULL;
    }

    char name[512];
    uint64_t n = ++upload_counter;
    int wrote = snprintf(name, sizeof(name), "%s/upload-%lld-%llu-%s",
                         dir, (long long)time(NULL), (unsigned long long)n, safe);
    free(safe);
    if (wrote < 0 || (size_t)wrote >= sizeof(name))
        return NULL;
    return http_strdup(name);
}

static void set_field(lua_State *L, int tidx, const char *k, size_t klen,
                      const char *v, size_t vlen) {
    lua_pushlstring(L, k, klen);
    lua_pushlstring(L, v, vlen);
    lua_settable(L, tidx);
}

static void cleanup_uploads(lua_State *L, int files_idx) {
    size_t n = (size_t)lua_objlen(L, files_idx);
    for (size_t i = 1; i <= n; i++) {
        lua_rawgeti(L, files_idx, (int)i);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "path");
            if (lua_isstring(L, -1))
                unlink(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

bool http_multipart_parse(http_conn_t *c, lua_State *L, int form_idx,
                          int files_idx) {
    if (!c || !L) return false;
    form_idx = absindex(L, form_idx);
    files_idx = absindex(L, files_idx);

    const char *ct = http_header_get(c->headers, c->nheaders, "content-type");
    str_t boundary = str_init(64);
    if (!get_boundary(ct, &boundary)) {
        str_deinit(&boundary);
        return false;
    }

    str_t marker = str_init(128);
    str_push(&marker, "--");
    str_push_len(&marker, boundary.data, boundary.len);

    const char *data = c->body.data ? c->body.data : "";
    size_t len = c->body.len;
    size_t found = find_bytes(data, len, marker.data, marker.len, 0);
    if (found == SIZE_MAX) {
        str_deinit(&boundary);
        str_deinit(&marker);
        return false;
    }

    size_t pos = found + marker.len;
    size_t max_parts = c->state ? c->state->cfg.max_multipart_parts : 64;
    size_t max_files = c->state ? c->state->cfg.max_multipart_files : 32;
    if (max_parts == 0) max_parts = 64;
    if (max_files == 0) max_files = 32;
    size_t parts = 0;
    size_t file_count = 0;
    bool ok = true;
    bool final = false;

    while (pos <= len) {
        if (pos + 2 <= len && data[pos] == '-' && data[pos + 1] == '-') {
            final = true;
            break;
        }

        if (pos + 2 <= len && data[pos] == '\r' && data[pos + 1] == '\n')
            pos += 2;
        else if (pos < len && data[pos] == '\n')
            pos += 1;
        else
            break;

        str_t delim = str_init(marker.len + 8);
        str_push(&delim, "\r\n");
        str_push_len(&delim, marker.data, marker.len);

        size_t next = find_bytes(data, len, delim.data, delim.len, pos);
        size_t dlen = delim.len;
        if (next == SIZE_MAX) {
            str_clear(&delim);
            str_push(&delim, "\n");
            str_push_len(&delim, marker.data, marker.len);
            dlen = delim.len;
            next = find_bytes(data, len, delim.data, delim.len, pos);
        }
        if (next == SIZE_MAX) {
            ok = false;
            str_deinit(&delim);
            break;
        }

        size_t hend = find_bytes(data + pos, next - pos, "\r\n\r\n", 4, 0);
        size_t hskip = 4;
        if (hend == SIZE_MAX) {
            hend = find_bytes(data + pos, next - pos, "\n\n", 2, 0);
            hskip = 2;
        }
        if (hend == SIZE_MAX) {
            ok = false;
            str_deinit(&delim);
            break;
        }

        const char *headers = data + pos;
        size_t hlen = hend;
        const char *part = data + pos + hend + hskip;
        size_t plen = next - (pos + hend + hskip);

        char *field = NULL;
        char *filename = NULL;
        char *ctype = NULL;
        if (!parse_part_headers(headers, hlen, &field, &filename, &ctype)) {
            free(field);
            free(filename);
            free(ctype);
            ok = false;
            str_deinit(&delim);
            break;
        }

        if (++parts > max_parts) {
            free(field);
            free(filename);
            free(ctype);
            ok = false;
            str_deinit(&delim);
            break;
        }

        if (filename && *filename) {
            if (++file_count > max_files) {
                free(field);
                free(filename);
                free(ctype);
                ok = false;
                str_deinit(&delim);
                break;
            }
            char *path = make_upload_path(c, filename);
            if (!path) {
                free(field);
                free(filename);
                free(ctype);
                ok = false;
                str_deinit(&delim);
                break;
            }
            FILE *f = fopen(path, "wb");
            bool wrote = false;
            if (f) {
                if (plen == 0 || fwrite(part, 1, plen, f) == plen)
                    wrote = true;
                fclose(f);
            }
            if (!wrote) {
                unlink(path);
                free(path);
                free(field);
                free(filename);
                free(ctype);
                ok = false;
                str_deinit(&delim);
                break;
            }

            lua_createtable(L, 0, 5);
            lua_pushstring(L, field);
            lua_setfield(L, -2, "field");
            lua_pushstring(L, filename);
            lua_setfield(L, -2, "filename");
            lua_pushstring(L, ctype ? ctype : "application/octet-stream");
            lua_setfield(L, -2, "content_type");
            lua_pushstring(L, path);
            lua_setfield(L, -2, "path");
            lua_pushinteger(L, (lua_Integer)plen);
            lua_setfield(L, -2, "size");
            lua_rawseti(L, files_idx, file_count);
            free(path);
        } else {
            set_field(L, form_idx, field, strlen(field), part, plen);
        }

        free(field);
        free(filename);
        free(ctype);
        str_deinit(&delim);

        pos = next + dlen;
    }

    if (ok && !final)
        ok = false;
    if (!ok)
        cleanup_uploads(L, files_idx);

    str_deinit(&boundary);
    str_deinit(&marker);
    return ok;
}
