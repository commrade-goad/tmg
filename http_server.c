#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static llhttp_settings_t http_settings;
static bool http_settings_ready = false;

static http_conn_t *conn_of(llhttp_t *p) {
    return (http_conn_t *)p->data;
}

static void conn_free(http_conn_t *c);
static void timer_cb(uv_timer_t *t);

static void stop_reading(http_conn_t *c) {
    if (c->reading) {
        uv_read_stop((uv_stream_t *)&c->handle);
        c->reading = false;
    }
}

static void flush_header(http_conn_t *c) {
    if (c->header_field.len == 0) return;
    http_headers_add(&c->headers, &c->nheaders, &c->cheaders,
                     c->header_field.data,
                     c->header_value.data ? c->header_value.data : "", false);
    str_clear(&c->header_field);
    str_clear(&c->header_value);
    c->header_value_active = false;
}

static int on_message_begin(llhttp_t *p) {
    http_conn_t *c = conn_of(p);
    http_conn_reset_request(c);
    return 0;
}

static int on_url(llhttp_t *p, const char *at, size_t len) {
    http_conn_t *c = conn_of(p);
    size_t max = c->state->cfg.max_url;
    if (max && c->url.len + len > max) {
        c->limit_status = 414;
        llhttp_set_error_reason(p, "request URL too long");
        return -1;
    }
    str_push_len(&c->url, at, len);
    return 0;
}

static int on_header_field(llhttp_t *p, const char *at, size_t len) {
    http_conn_t *c = conn_of(p);
    if (c->header_value_active)
        flush_header(c);
    if (c->nheaders >= c->state->cfg.max_headers) {
        c->limit_status = 431;
        llhttp_set_error_reason(p, "too many headers");
        return -1;
    }
    if (c->header_bytes + len > c->state->cfg.max_header_bytes) {
        c->limit_status = 431;
        llhttp_set_error_reason(p, "header bytes limit exceeded");
        return -1;
    }
    c->header_bytes += len;
    str_push_len(&c->header_field, at, len);
    return 0;
}

static int on_header_value(llhttp_t *p, const char *at, size_t len) {
    http_conn_t *c = conn_of(p);
    if (c->header_bytes + len > c->state->cfg.max_header_bytes) {
        c->limit_status = 431;
        llhttp_set_error_reason(p, "header bytes limit exceeded");
        return -1;
    }
    c->header_bytes += len;
    str_push_len(&c->header_value, at, len);
    c->header_value_active = true;
    return 0;
}

typedef struct {
    uv_write_t req;
    char *buf;
} http_small_write_t;

static void small_write_cb(uv_write_t *req, int status) {
    (void)status;
    http_small_write_t *w = (http_small_write_t *)req->data;
    free(w->buf);
    free(w);
}

static void send_continue(http_conn_t *c) {
    if (c->closing) return;
    http_small_write_t *w = calloc(1, sizeof(*w));
    if (!w) return;
    w->buf = http_strdup("HTTP/1.1 100 Continue\r\n\r\n");
    w->req.data = w;
    uv_buf_t buf = uv_buf_init(w->buf, (unsigned int)strlen(w->buf));
    int rc = uv_write(&w->req, (uv_stream_t *)&c->handle, &buf, 1, small_write_cb);
    if (rc != 0) {
        free(w->buf);
        free(w);
    }
}

static int on_headers_complete(llhttp_t *p) {
    http_conn_t *c = conn_of(p);
    flush_header(c);
    c->headers_complete = true;

    free(c->method);
    free(c->version);
    const char *m = llhttp_method_name((llhttp_method_t)p->method);
    c->method = http_strdup(m ? m : "GET");

    char ver[32];
    snprintf(ver, sizeof(ver), "HTTP/%d.%d", p->http_major, p->http_minor);
    c->version = http_strdup(ver);

    c->keep_alive = llhttp_should_keep_alive(p) != 0;

    const char *cl = http_header_get(c->headers, c->nheaders, "content-length");
    if (cl && *cl) {
        char *end = NULL;
        errno = 0;
        unsigned long long n = strtoull(cl, &end, 10);
        if (errno == 0 && end && *end == '\0' && c->state->cfg.max_body && n > c->state->cfg.max_body) {
            c->limit_status = 413;
            llhttp_set_error_reason(p, "request body too large");
            return -1;
        }
    }

    const char *expect = http_header_get(c->headers, c->nheaders, "expect");
    if (expect) {
        if (strcasecmp(expect, "100-continue") == 0) {
            c->expect_continue = true;
            send_continue(c);
        } else {
            c->limit_status = 417;
            llhttp_set_error_reason(p, "unsupported Expect header");
            return -1;
        }
    }

    return 0;
}

static int on_body(llhttp_t *p, const char *at, size_t len) {
    http_conn_t *c = conn_of(p);
    size_t max = c->state->cfg.max_body;
    if (max && c->body.len + len > max) {
        c->limit_status = 413;
        llhttp_set_error_reason(p, "request body too large");
        return -1;
    }
    str_push_len(&c->body, at, len);
    return 0;
}

static int on_message_complete(llhttp_t *p) {
    http_conn_t *c = conn_of(p);
    c->message_complete = true;
    c->keepalive_wait = false;
    stop_reading(c);
    if (c->timer_init)
        uv_timer_stop(&c->timer);

    if (llhttp_get_upgrade(p)) {
        /* llhttp pauses with HPE_PAUSED_UPGRADE after this callback.  Dispatch
         * from on_read so any bytes already read after the HTTP headers can be
         * handed to wslay without going through the HTTP parser again. */
        return 0;
    }

    llhttp_pause(p);
    c->handler_running = true;
    http_dispatch(c);
    c->handler_running = false;
    return 0;
}

static void on_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    (void)handle;
    (void)suggested;
    char *p = malloc(HTTP_READ_CHUNK);
    if (!p) {
        *buf = uv_buf_init(NULL, 0);
        return;
    }
    *buf = uv_buf_init(p, HTTP_READ_CHUNK);
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    http_conn_t *c = (http_conn_t *)stream->data;

    if (nread < 0) {
        free(buf->base);
        if (nread != UV_EOF)
            http_log("read error: %s", uv_strerror((int)nread));
        http_conn_close(c);
        return;
    }
    if (nread == 0) {
        free(buf->base);
        return;
    }
    if (c->closing) {
        free(buf->base);
        return;
    }
    if (c->ws) {
        http_ws_feed(c, buf->base, (size_t)nread);
        free(buf->base);
        return;
    }

    c->keepalive_wait = false;
    if (c->timer_init) {
        unsigned timeout = c->state->cfg.header_timeout;
        if (timeout > 0)
            uv_timer_start(&c->timer, timer_cb, (uint64_t)timeout * 1000, 0);
        else
            uv_timer_stop(&c->timer);
    }

    llhttp_errno_t err = llhttp_execute(&c->parser, buf->base, (size_t)nread);
    if (err == HPE_OK) {
        /* parser is still waiting for more data */
    } else if (err == HPE_PAUSED) {
        const char *pos = llhttp_get_error_pos(&c->parser);
        if (pos && pos < buf->base + nread) {
            /* pipelined data is not supported: finish this response, then close */
            c->close_after_write = true;
        }
    } else if (err == HPE_PAUSED_UPGRADE) {
        const char *pos = llhttp_get_error_pos(&c->parser);
        size_t used = pos ? (size_t)(pos - buf->base) : (size_t)nread;
        if (used > (size_t)nread) used = (size_t)nread;

        c->handler_running = true;
        http_dispatch(c);
        c->handler_running = false;

        if (c->ws) {
            if (used < (size_t)nread)
                http_ws_feed(c, buf->base + used, (size_t)nread - used);
            if (!c->closing && !c->reading && !c->close_after_write) {
                int rc = uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
                if (rc != 0) {
                    http_log("uv_read_start failed: %s", uv_strerror(rc));
                    http_conn_close(c);
                } else {
                    c->reading = true;
                }
            }
        } else {
            c->close_after_write = true;
        }
    } else {
        int status = c->limit_status ? c->limit_status : 400;
        const char *reason = llhttp_get_error_reason(&c->parser);
        if (!reason) reason = llhttp_errno_name(err);
        http_log("parse error: %s", reason ? reason : "bad request");
        http_conn_send_error(c, status, http_status_reason(status));
    }

    free(buf->base);
}

void http_conn_write_cb(uv_write_t *req, int status) {
    http_conn_t *c = (http_conn_t *)req->data;
    c->writing = false;
    free(c->write_buf);
    c->write_buf = NULL;
    c->write_len = 0;

    if (c->closing) return;
    if (status < 0) {
        http_log("write error: %s", uv_strerror(status));
        http_conn_close(c);
        return;
    }
    if (c->close_after_write || !c->keep_alive) {
        http_conn_close(c);
        return;
    }

    http_response_reset(c);
    http_conn_reset_request(c);
    llhttp_resume(&c->parser);

    c->keepalive_wait = true;
    unsigned keepalive = c->state->cfg.keepalive_timeout;
    if (c->timer_init) {
        if (keepalive > 0)
            uv_timer_start(&c->timer, timer_cb, (uint64_t)keepalive * 1000, 0);
        else
            uv_timer_stop(&c->timer);
    }

    int rc = uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
    if (rc != 0) {
        http_log("uv_read_start failed: %s", uv_strerror(rc));
        http_conn_close(c);
        return;
    }
    c->reading = true;
}

static void timer_cb(uv_timer_t *t) {
    http_conn_t *c = (http_conn_t *)t->data;
    if (c->closing) return;
    if (c->keepalive_wait) {
        http_conn_close(c);
        return;
    }
    if (!c->message_complete && !c->handler_running) {
        http_conn_send_error(c, 408, http_status_reason(408));
    } else {
        http_conn_close(c);
    }
}

static void on_timer_close(uv_handle_t *h) {
    http_conn_t *c = (http_conn_t *)h->data;
    if (--c->close_refs == 0)
        conn_free(c);
}

static void on_handle_close(uv_handle_t *h) {
    http_conn_t *c = (http_conn_t *)h->data;
    if (--c->close_refs == 0)
        conn_free(c);
}

static void conn_free(http_conn_t *c) {
    if (!c) return;
    http_state_t *st = c->state;
    if (st) {
        http_conn_t **pp = &st->conns;
        while (*pp) {
            if (*pp == c) {
                *pp = c->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    http_ws_cleanup(c);
    free(c->method);
    free(c->version);
    free(c->write_buf);
    str_deinit(&c->url);
    str_deinit(&c->header_field);
    str_deinit(&c->header_value);
    str_deinit(&c->body);
    str_deinit(&c->res_body);
    http_headers_free(c->headers, c->nheaders);
    http_headers_free(c->res_headers, c->nres_headers);
    free(c);
}

void http_conn_close(http_conn_t *c) {
    if (!c || c->closing) return;
    c->closing = true;
    stop_reading(c);
    http_ws_disconnect(c);

    if (c->timer_init && !c->timer_closing) {
        c->timer_closing = true;
        uv_timer_stop(&c->timer);
        c->close_refs++;
        uv_close((uv_handle_t *)&c->timer, on_timer_close);
    }
    if (!c->handle_closing) {
        c->handle_closing = true;
        c->close_refs++;
        uv_close((uv_handle_t *)&c->handle, on_handle_close);
    }
    if (c->close_refs == 0)
        conn_free(c);
}

void http_conn_reset_request(http_conn_t *c) {
    if (!c) return;
    free(c->method);
    free(c->version);
    c->method = NULL;
    c->version = NULL;
    str_clear(&c->url);
    str_clear(&c->header_field);
    str_clear(&c->header_value);
    str_clear(&c->body);
    http_headers_free(c->headers, c->nheaders);
    c->headers = NULL;
    c->nheaders = 0;
    c->cheaders = 0;
    c->header_value_active = false;
    c->headers_complete = false;
    c->message_complete = false;
    c->keep_alive = false;
    c->expect_continue = false;
    c->limit_status = 0;
    c->header_bytes = 0;
}

static void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        http_log("accept error: %s", uv_strerror(status));
        return;
    }
    http_state_t *st = (http_state_t *)server->data;

    http_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        http_log("out of memory accepting connection");
        return;
    }
    c->state = st;
    c->status = 200;
    c->url = str_init(128);
    c->header_field = str_init(32);
    c->header_value = str_init(32);
    c->body = str_init(128);
    c->res_body = str_init(128);

    c->handle.data = c;
    c->timer.data = c;
    if (uv_tcp_init(st->loop, &c->handle) != 0) {
        conn_free(c);
        return;
    }
    c->close_refs = 0;

    if (uv_accept(server, (uv_stream_t *)&c->handle) != 0) {
        http_conn_close(c);
        return;
    }

    c->next = st->conns;
    st->conns = c;

    struct sockaddr_storage ss;
    int slen = sizeof(ss);
    if (uv_tcp_getpeername((uv_tcp_t *)&c->handle, (struct sockaddr *)&ss, &slen) == 0) {
        char ip[INET6_ADDRSTRLEN] = "?";
        int port = 0;
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ss;
            uv_ip4_name(a, ip, sizeof(ip));
            port = ntohs(a->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            uv_ip6_name(a, ip, sizeof(ip));
            port = ntohs(a->sin6_port);
        }
        snprintf(c->remote, sizeof(c->remote), "%s:%d", ip, port);
    }

    llhttp_init(&c->parser, HTTP_REQUEST, &http_settings);
    c->parser.data = c;

    if (uv_timer_init(st->loop, &c->timer) != 0) {
        http_conn_close(c);
        return;
    }
    c->timer_init = true;

    int rc = uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
    if (rc != 0) {
        http_log("uv_read_start failed: %s", uv_strerror(rc));
        http_conn_close(c);
        return;
    }
    c->reading = true;

    unsigned timeout = st->cfg.header_timeout;
    if (timeout > 0)
        uv_timer_start(&c->timer, timer_cb, (uint64_t)timeout * 1000, 0);
}

static void on_server_close(uv_handle_t *h) {
    (void)h;
}

static void close_server(http_state_t *st) {
    if (st->server_init && !st->server_closing) {
        st->server_closing = true;
        uv_close((uv_handle_t *)&st->server, on_server_close);
    }
}

static void close_signals(http_state_t *st) {
    if (st->sigint_init) {
        uv_signal_stop(&st->sigint);
        uv_close((uv_handle_t *)&st->sigint, NULL);
        st->sigint_init = false;
    }
    if (st->sigterm_init) {
        uv_signal_stop(&st->sigterm);
        uv_close((uv_handle_t *)&st->sigterm, NULL);
        st->sigterm_init = false;
    }
}

static void on_signal(uv_signal_t *handle, int signum) {
    (void)signum;
    http_state_t *st = (http_state_t *)handle->data;
    http_state_stop(st);
}

http_state_t *http_state_get(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, HTTP_STATE_KEY);
    http_state_t *st = lua_isuserdata(L, -1) ? (http_state_t *)lua_touserdata(L, -1) : NULL;
    lua_pop(L, 1);
    return st;
}

int http_state_start(lua_State *L, http_state_t *st) {
    (void)L;
    if (!st) return UV_EINVAL;
    if (st->listening) return UV_EALREADY;

    if (!http_settings_ready) {
        llhttp_settings_init(&http_settings);
        http_settings.on_message_begin = on_message_begin;
        http_settings.on_url = on_url;
        http_settings.on_header_field = on_header_field;
        http_settings.on_header_value = on_header_value;
        http_settings.on_headers_complete = on_headers_complete;
        http_settings.on_body = on_body;
        http_settings.on_message_complete = on_message_complete;
        http_settings_ready = true;
    }

    st->loop = uv_default_loop();
    if (!st->loop) {
        http_log("failed to create libuv loop");
        return UV_ENOMEM;
    }

    if (uv_tcp_init(st->loop, &st->server) != 0)
        return UV_ENOMEM;
    st->server_init = true;
    st->server.data = st;

    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    const char *host = st->cfg.host ? st->cfg.host : "127.0.0.1";
    if (strcmp(host, "localhost") == 0)
        host = "127.0.0.1";

    int rc;
    if (strchr(host, ':')) {
        rc = uv_ip6_addr(host, st->cfg.port, &addr6);
        if (rc == 0)
            rc = uv_tcp_bind(&st->server, (const struct sockaddr *)&addr6, 0);
    } else {
        rc = uv_ip4_addr(host, st->cfg.port, &addr4);
        if (rc == 0)
            rc = uv_tcp_bind(&st->server, (const struct sockaddr *)&addr4, 0);
    }
    if (rc != 0) {
        http_log("bind failed on %s:%d: %s", host, st->cfg.port, uv_strerror(rc));
        close_server(st);
        return rc;
    }

    rc = uv_listen((uv_stream_t *)&st->server, 128, on_new_connection);
    if (rc != 0) {
        http_log("listen failed: %s", uv_strerror(rc));
        close_server(st);
        return rc;
    }

    st->sigint.data = st;
    st->sigterm.data = st;
    if (uv_signal_init(st->loop, &st->sigint) == 0) {
        st->sigint_init = true;
        uv_signal_start(&st->sigint, on_signal, SIGINT);
    }
    if (uv_signal_init(st->loop, &st->sigterm) == 0) {
        st->sigterm_init = true;
        uv_signal_start(&st->sigterm, on_signal, SIGTERM);
    }

    st->listening = true;
    st->stopping = false;
    return 0;
}

void http_state_stop(http_state_t *st) {
    if (!st || st->stopping) return;
    st->stopping = true;
    st->listening = false;
    close_server(st);
    close_signals(st);

    for (http_conn_t *c = st->conns; c; c = c->next) {
        if (c->closing) continue;
        if (c->writing || c->handler_running)
            c->close_after_write = true;
        else
            http_conn_close(c);
    }
}

void http_state_run(lua_State *L) {
    http_state_t *st = http_state_get(L);
    if (!st || !st->listening) return;
    uv_run(st->loop, UV_RUN_DEFAULT);
    st->listening = false;
}

void http_state_deinit(lua_State *L) {
    http_state_t *st = http_state_get(L);
    if (!st) return;

    http_state_stop(st);

    for (size_t i = 0; i < st->nroutes; i++) {
        if (st->routes[i].handler_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, st->routes[i].handler_ref);
    }
    http_routes_free(st->routes, st->nroutes);
    st->routes = NULL;
    st->nroutes = 0;
    st->croutes = 0;

    http_statics_free(st->statics, st->nstatics);
    st->statics = NULL;
    st->nstatics = 0;
    st->cstatics = 0;

    free(st->cfg.host);
    free(st->cfg.upload_dir);
    free(st->cfg.template_root);
    memset(&st->cfg, 0, sizeof(st->cfg));

    if (st->loop) {
        uv_run(st->loop, UV_RUN_NOWAIT);
        int rc = uv_loop_close(st->loop);
        if (rc != 0)
            http_log("uv_loop_close: %s", uv_strerror(rc));
        st->loop = NULL;
    }
    st->server_init = false;
    st->listening = false;
}
