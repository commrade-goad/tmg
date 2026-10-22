#include "http.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static void static_error(http_conn_t *c, int status, const char *allow) {
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

static void set_err(str_t *err, const char *msg) {
    if (!err) return;
    str_clear(err);
    str_push(err, msg);
}

bool http_static_add(http_state_t *st, const char *prefix, const char *root,
                     str_t *err) {
    if (!st || !prefix || prefix[0] != '/') {
        set_err(err, "prefix must start with '/'");
        return false;
    }
    if (!root || !*root)
        root = ".";

    size_t plen = strlen(prefix);
    while (plen > 1 && prefix[plen - 1] == '/')
        plen--;
    if (strstr(prefix, "..")) {
        set_err(err, "prefix must not contain '..'");
        return false;
    }

    if (st->nstatics >= st->cstatics) {
        size_t nc = st->cstatics ? st->cstatics * 2 : 4;
        http_static_t *p = http_realloc(st->statics, sizeof(*p) * nc);
        st->statics = p;
        st->cstatics = nc;
    }

    st->statics[st->nstatics].prefix = http_strndup(prefix, plen);
    st->statics[st->nstatics].root = http_strdup(root);
    st->nstatics++;
    return true;
}

void http_statics_free(http_static_t *statics, size_t n) {
    if (!statics) return;
    for (size_t i = 0; i < n; i++) {
        free(statics[i].prefix);
        free(statics[i].root);
    }
    free(statics);
}

static const char *match_prefix(const char *prefix, const char *path) {
    size_t plen = strlen(prefix);
    if (plen == 1 && prefix[0] == '/')
        return path[0] == '/' ? path + 1 : path;
    if (strncmp(path, prefix, plen) != 0)
        return NULL;
    if (path[plen] != '\0' && path[plen] != '/')
        return NULL;
    const char *rel = path + plen;
    if (*rel == '/')
        rel++;
    return rel;
}

static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0 || strcasecmp(dot, ".mjs") == 0)
        return "text/javascript; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0)
        return "application/json; charset=utf-8";
    if (strcasecmp(dot, ".txt") == 0)
        return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".xml") == 0)
        return "application/xml; charset=utf-8";
    if (strcasecmp(dot, ".svg") == 0)
        return "image/svg+xml; charset=utf-8";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(dot, ".wasm") == 0) return "application/wasm";
    if (strcasecmp(dot, ".woff") == 0) return "font/woff";
    if (strcasecmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcasecmp(dot, ".ttf") == 0) return "font/ttf";
    if (strcasecmp(dot, ".otf") == 0) return "font/otf";
    if (strcasecmp(dot, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(dot, ".webm") == 0) return "video/webm";
    if (strcasecmp(dot, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(dot, ".wav") == 0) return "audio/wav";
    if (strcasecmp(dot, ".zip") == 0) return "application/zip";
    if (strcasecmp(dot, ".gz") == 0) return "application/gzip";
    return "application/octet-stream";
}

static bool resolve_file(const char *root, const char *rel, char *out,
                         size_t outsz) {
    str_t full = str_init(strlen(rel) + strlen(root) + 2);
    bool ok = http_path_safe(root, rel, &full);
    if (!ok) {
        str_deinit(&full);
        return false;
    }

    char real_root[PATH_MAX];
    char real_file[PATH_MAX];
    if (!realpath(root, real_root) || !realpath(full.data, real_file)) {
        str_deinit(&full);
        return false;
    }

    size_t rl = strlen(real_root);
    bool inside = strncmp(real_file, real_root, rl) == 0 &&
                  (real_file[rl] == '/' || real_file[rl] == '\0');
    if (!inside) {
        str_deinit(&full);
        return false;
    }

    int n = snprintf(out, outsz, "%s", real_file);
    str_deinit(&full);
    return n > 0 && (size_t)n < outsz;
}

bool http_static_serve(http_conn_t *c, const char *method, const char *path) {
    if (!c || !c->state || !method || !path) return false;

    bool get = strcmp(method, "GET") == 0;
    bool head = strcmp(method, "HEAD") == 0;

    for (size_t i = 0; i < c->state->nstatics; i++) {
        http_static_t *mount = &c->state->statics[i];
        const char *rel = match_prefix(mount->prefix, path);
        if (!rel) continue;

        if (!get && !head) {
            static_error(c, 405, "GET, HEAD");
            return true;
        }

        str_t wanted = str_init(strlen(rel) + 16);
        str_push(&wanted, rel);
        if (wanted.len == 0 || wanted.data[wanted.len - 1] == '/')
            str_push(&wanted, "index.html");

        char file[PATH_MAX];
        bool ok = resolve_file(mount->root, wanted.data, file, sizeof(file));
        str_deinit(&wanted);
        if (!ok) {
            static_error(c, 404, NULL);
            return true;
        }

        struct stat st;
        if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
            static_error(c, 404, NULL);
            return true;
        }
        if (c->state->cfg.max_static_size &&
            (uintmax_t)st.st_size > (uintmax_t)c->state->cfg.max_static_size) {
            http_log("static file too large: %s", file);
            static_error(c, 500, NULL);
            return true;
        }

        FILE *f = fopen(file, "rb");
        if (!f) {
            static_error(c, 404, NULL);
            return true;
        }

        http_response_reset(c);
        c->status = 200;
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Content-Type", mime_type(file), true);
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "X-Content-Type-Options", "nosniff", true);

        char lm[64];
        struct tm tmv;
        if (gmtime_r(&st.st_mtime, &tmv) &&
            strftime(lm, sizeof(lm), "%a, %d %b %Y %H:%M:%S GMT", &tmv) > 0)
            http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                             "Last-Modified", lm, true);

        str_clear(&c->res_body);
        char buf[65536];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (c->state->cfg.max_static_size &&
                c->res_body.len + got > c->state->cfg.max_static_size) {
                fclose(f);
                http_log("static file grew too large: %s", file);
                static_error(c, 500, NULL);
                return true;
            }
            str_push_len(&c->res_body, buf, got);
        }
        if (ferror(f)) {
            fclose(f);
            http_log("static file read failed: %s", file);
            static_error(c, 500, NULL);
            return true;
        }
        fclose(f);

        c->ended = true;
        http_conn_send_response(c);
        return true;
    }
    return false;
}
