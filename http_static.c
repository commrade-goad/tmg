#include "http.h"

#include <fcntl.h>
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

static void static_free_buffers(http_conn_t *c) {
    free(c->static_path);
    c->static_path = NULL;
    free(c->static_buf);
    c->static_buf = NULL;
    c->static_size = 0;
}

static void static_close_fd(http_conn_t *c) {
    if (c->static_fd >= 0) {
        uv_fs_t req;
        uv_fs_close(NULL, &req, c->static_fd, NULL);
        uv_fs_req_cleanup(&req);
        c->static_fd = -1;
    }
}

void http_static_cleanup(http_conn_t *c) {
    if (!c) return;
    static_close_fd(c);
    static_free_buffers(c);
    c->static_fs_active = false;
    c->static_fs_ref = false;
    c->static_streaming = false;
    c->static_headers_sent = false;
    c->static_cancel = false;
    c->static_head = false;
}

void http_static_cancel(http_conn_t *c) {
    if (!c) return;
    c->static_cancel = true;
    if (c->static_fs_active) {
        uv_cancel((uv_req_t *)&c->static_req);
        return;
    }
    static_close_fd(c);
    static_free_buffers(c);
    c->static_streaming = false;
    c->static_headers_sent = false;
    if (c->static_fs_ref) {
        c->static_fs_ref = false;
        http_conn_release_ref(c);
    }
}

static void static_fs_acquire(http_conn_t *c) {
    if (!c->static_fs_ref) {
        c->static_fs_ref = true;
        c->close_refs++;
    }
}

static void static_fs_release(http_conn_t *c) {
    if (c->static_fs_ref) {
        c->static_fs_ref = false;
        http_conn_release_ref(c);
    }
}

static void static_add_headers(http_conn_t *c, const char *file,
                               const uv_stat_t *st) {
    http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                     "Content-Type", mime_type(file), true);
    http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                     "X-Content-Type-Options", "nosniff", true);

    char lm[64];
    struct tm tmv;
    time_t mtime = (time_t)st->st_mtim.tv_sec;
    if (gmtime_r(&mtime, &tmv) &&
        strftime(lm, sizeof(lm), "%a, %d %b %Y %H:%M:%S GMT", &tmv) > 0)
        http_headers_add(&c->res_headers, &c->nres_headers, &c->cres_headers,
                         "Last-Modified", lm, true);
}

static void static_abort(http_conn_t *c, int status, const char *msg) {
    bool closing = c->closing;
    bool headers = c->static_headers_sent;
    static_close_fd(c);
    static_free_buffers(c);
    c->static_streaming = false;
    c->static_headers_sent = false;
    static_fs_release(c);
    if (closing) return;
    if (headers) {
        http_conn_close(c);
    } else {
        if (msg && *msg)
            http_conn_send_error(c, status, msg);
        else
            static_error(c, status, NULL);
    }
}

static void static_read_next(http_conn_t *c);

static void static_chunk_cb(uv_write_t *req, int status) {
    http_conn_t *c = (http_conn_t *)req->data;
    http_conn_write_cleanup(c);
    if (!http_conn_write_release(c))
        return;
    if (status < 0) {
        http_log("static chunk write failed: %s", uv_strerror(status));
        static_abort(c, 500, NULL);
        return;
    }
    static_read_next(c);
}

static bool static_write_chunk(http_conn_t *c, const char *data, size_t len,
                               uv_write_cb cb) {
    char header[32];
    int n = snprintf(header, sizeof(header), "%zx\r\n", len);
    if (n < 0 || len > SIZE_MAX - (size_t)n - 2) return false;

    size_t total = (size_t)n + len + 2;
    char *buf = malloc(total);
    if (!buf) return false;
    memcpy(buf, header, (size_t)n);
    memcpy(buf + n, data, len);
    buf[n + len] = '\r';
    buf[n + len + 1] = '\n';
    return http_conn_write_raw(c, buf, total, cb);
}

static void static_write_final(http_conn_t *c) {
    static_close_fd(c);
    static_free_buffers(c);
    c->static_streaming = false;
    c->static_headers_sent = false;
    static_fs_release(c);
    if (c->closing) return;

    char *buf = malloc(5);
    if (!buf) {
        http_conn_close(c);
        return;
    }
    memcpy(buf, "0\r\n\r\n", 5);
    if (!http_conn_write_raw(c, buf, 5, http_conn_write_cb))
        http_conn_close(c);
}

static void static_read_cb(uv_fs_t *req) {
    http_conn_t *c = (http_conn_t *)req->data;
    if (!c) return;
    int rc = (int)req->result;
    uv_fs_req_cleanup(req);
    c->static_fs_active = false;

    if (c->closing || c->static_cancel) {
        static_close_fd(c);
        static_free_buffers(c);
        c->static_streaming = false;
        c->static_headers_sent = false;
        static_fs_release(c);
        return;
    }

    if (rc < 0) {
        http_log("static read failed: %s", uv_strerror(rc));
        static_abort(c, 500, "static file read failed");
        return;
    }
    if (rc == 0) {
        static_write_final(c);
        return;
    }

    uint64_t add = (uint64_t)rc;
    uint64_t max_static = (uint64_t)c->state->cfg.max_static_size;
    uint64_t max_response = (uint64_t)c->state->cfg.max_response;
    bool too_large = (max_static &&
                      (add > max_static || c->static_size > max_static - add)) ||
                     (max_response &&
                      (add > max_response ||
                       c->static_size > max_response - add));
    if (too_large) {
        http_log("static file grew past its size limit: %s", c->static_path);
        static_abort(c, 500, "static file too large");
        return;
    }
    c->static_size += add;
    if (!static_write_chunk(c, c->static_buf, (size_t)rc, static_chunk_cb)) {
        static_abort(c, 500, NULL);
    }
}

static void static_read_next(http_conn_t *c) {
    if (c->closing || c->static_cancel) {
        static_abort(c, 500, NULL);
        return;
    }
    if (!c->static_buf) {
        c->static_buf = malloc(HTTP_STATIC_CHUNK);
        if (!c->static_buf) {
            static_abort(c, 500, NULL);
            return;
        }
    }

    c->static_fs_active = true;
    c->static_req.data = c;
    uv_buf_t b = uv_buf_init(c->static_buf, HTTP_STATIC_CHUNK);
    http_conn_timer_set(c, HTTP_TIMER_STATIC, c->state->cfg.write_timeout);
    int rc = uv_fs_read(uv_default_loop(), &c->static_req, c->static_fd,
                        &b, 1, -1, static_read_cb);
    if (rc < 0) {
        c->static_fs_active = false;
        http_log("static read start failed: %s", uv_strerror(rc));
        static_abort(c, 500, "static file read failed");
    }
}

static void static_head_cb(uv_write_t *req, int status) {
    http_conn_t *c = (http_conn_t *)req->data;
    http_conn_write_cleanup(c);
    if (!http_conn_write_release(c))
        return;
    if (status < 0) {
        http_log("static head write failed: %s", uv_strerror(status));
        static_abort(c, 500, NULL);
        return;
    }
    c->static_headers_sent = true;
    static_read_next(c);
}

static void static_open_cb(uv_fs_t *req) {
    http_conn_t *c = (http_conn_t *)req->data;
    if (!c) return;
    int rc = (int)req->result;
    uv_fs_req_cleanup(req);
    c->static_fs_active = false;

    if (c->closing || c->static_cancel) {
        if (rc >= 0) {
            uv_fs_t close_req;
            uv_fs_close(NULL, &close_req, (uv_file)rc, NULL);
            uv_fs_req_cleanup(&close_req);
        }
        static_free_buffers(c);
        c->static_streaming = false;
        static_fs_release(c);
        return;
    }

    if (rc < 0) {
        http_log("static open failed: %s", uv_strerror(rc));
        static_abort(c, 404, NULL);
        return;
    }

    c->static_fd = (uv_file)rc;
    c->status = 200;
    static_add_headers(c, c->static_path, &c->static_stat);
    c->ended = true;
    if (!http_conn_send_stream_head(c, static_head_cb))
        static_abort(c, 500, NULL);
}

static void static_stat_cb(uv_fs_t *req) {
    http_conn_t *c = (http_conn_t *)req->data;
    if (!c) return;
    int rc = (int)req->result;
    if (rc >= 0)
        c->static_stat = req->statbuf;
    uv_fs_req_cleanup(req);
    c->static_fs_active = false;

    if (c->closing || c->static_cancel) {
        static_free_buffers(c);
        c->static_streaming = false;
        static_fs_release(c);
        return;
    }

    if (rc < 0 || !S_ISREG(c->static_stat.st_mode)) {
        static_abort(c, 404, NULL);
        return;
    }
    if (c->state->cfg.max_static_size &&
        (uintmax_t)c->static_stat.st_size >
            (uintmax_t)c->state->cfg.max_static_size) {
        http_log("static file too large: %s", c->static_path);
        static_abort(c, 500, "static file too large");
        return;
    }
    if (c->state->cfg.max_response &&
        (uintmax_t)c->static_stat.st_size >
            (uintmax_t)c->state->cfg.max_response) {
        http_log("static file exceeds max_response: %s", c->static_path);
        static_abort(c, 500, "static file too large");
        return;
    }
    if (c->static_head) {
        c->static_size = (uint64_t)c->static_stat.st_size;
        c->status = 200;
        static_add_headers(c, c->static_path, &c->static_stat);
        c->res_content_length = (size_t)c->static_size;
        c->res_content_length_set = true;
        c->ended = true;
        static_free_buffers(c);
        c->static_streaming = false;
        static_fs_release(c);
        http_conn_send_response(c);
        return;
    }

    c->static_size = 0;
    c->static_fs_active = true;
    c->static_req.data = c;
    http_conn_timer_set(c, HTTP_TIMER_STATIC, c->state->cfg.write_timeout);
    int open_rc = uv_fs_open(uv_default_loop(), &c->static_req,
                             c->static_path, O_RDONLY, 0, static_open_cb);
    if (open_rc < 0) {
        c->static_fs_active = false;
        http_log("static open start failed: %s", uv_strerror(open_rc));
        static_abort(c, 404, NULL);
    }
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

        c->static_path = http_strdup(file);
        if (!c->static_path) {
            static_error(c, 500, NULL);
            return true;
        }
        c->static_fd = -1;
        c->static_buf = NULL;
        c->static_size = 0;
        c->static_head = head;
        c->static_streaming = true;
        c->static_headers_sent = false;
        c->static_cancel = false;
        static_fs_acquire(c);

        c->static_fs_active = true;
        c->static_req.data = c;
        http_conn_timer_set(c, HTTP_TIMER_STATIC, c->state->cfg.write_timeout);
        int rc = uv_fs_stat(uv_default_loop(), &c->static_req,
                            c->static_path, static_stat_cb);
        if (rc < 0) {
            c->static_fs_active = false;
            http_log("static stat start failed: %s", uv_strerror(rc));
            static_abort(c, 404, NULL);
        }
        return true;
    }
    return false;
}
