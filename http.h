#ifndef TMG_HTTP_H
#define TMG_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <lua.h>
#include <lauxlib.h>
#include <llhttp.h>
#include <uv.h>

#include "str.h"
#include "template_cache.h"

#define HTTP_STATE_KEY "tmg.http.state"
#define HTTP_RES_MT "tmg.http.response"
#define HTTP_REQ_MT "tmg.http.request"
#define HTTP_SERVER_NAME "tmg"
#define HTTP_READ_CHUNK 16384
#define HTTP_JSON_DEFAULT_DEPTH 64
#define HTTP_JSON_MAX_DEPTH 1024
#define HTTP_TIMER_NONE 0u
#define HTTP_TIMER_HEADER 1u
#define HTTP_TIMER_BODY 2u
#define HTTP_TIMER_HANDLER 3u
#define HTTP_TIMER_WRITE 4u
#define HTTP_TIMER_KEEPALIVE 5u
#define HTTP_TIMER_STATIC 6u
#define HTTP_STATIC_CHUNK 65536

typedef struct {
    char *name;
    char *value;
} http_header_t;

typedef struct http_conn http_conn_t;
typedef struct http_state http_state_t;
typedef struct http_ws http_ws_t;
typedef struct http_res_ud http_res_ud_t;

typedef struct {
    char *method;
    char *pattern;
    char **segs;
    size_t nsegs;
    bool dynamic;
    int handler_ref;
} http_route_t;

typedef struct {
    char *prefix;
    char *root;
} http_static_t;

typedef struct {
    char *host;
    int port;
    size_t max_body;
    size_t max_header_bytes;
    size_t max_headers;
    size_t max_url;
    size_t max_static_size;
    size_t max_ws_message;
    size_t max_response;
    unsigned header_timeout;
    unsigned body_timeout;
    unsigned handler_timeout;
    unsigned write_timeout;
    unsigned keepalive_timeout;
    char *upload_dir;
    char *template_root;
    size_t template_cache_max;
    size_t max_multipart_parts;
    size_t max_multipart_files;
    int template_cache_mode;
    bool debug;
} http_config_t;

struct http_state {
    uv_loop_t *loop;
    uv_tcp_t server;
    bool server_init;
    bool server_closing;
    bool listening;
    bool stopping;
    bool deinitialized;
    uv_signal_t sigint;
    uv_signal_t sigterm;
    bool sigint_init;
    bool sigterm_init;
    http_config_t cfg;
    http_route_t *routes;
    size_t nroutes;
    size_t croutes;
    http_static_t *statics;
    size_t nstatics;
    size_t cstatics;
    tmg_template_cache_t *template_cache;
    http_conn_t *conns;
    lua_State *L;
};

struct http_conn {
    uv_tcp_t handle;
    uv_timer_t timer;
    bool timer_init;
    bool timer_closing;
    bool handle_closing;
    int close_refs;
    http_state_t *state;
    llhttp_t parser;
    struct http_conn *next;

    /* request */
    str_t url;
    str_t header_field;
    str_t header_value;
    bool header_value_active;
    char *method;
    char *version;
    http_header_t *headers;
    size_t nheaders;
    size_t cheaders;
    str_t body;
    bool headers_complete;
    bool message_complete;
    bool keep_alive;
    bool expect_continue;
    bool keepalive_wait;
    int limit_status;
    size_t header_bytes;

    /* response */
    int status;
    http_header_t *res_headers;
    size_t nres_headers;
    size_t cres_headers;
    str_t res_body;
    size_t res_content_length;
    bool res_content_length_set;
    bool ended;
    bool sending_error;
    bool close_after_write;

    /* static file streaming */
    uv_fs_t static_req;
    uv_stat_t static_stat;
    uv_file static_fd;
    char *static_path;
    char *static_buf;
    uint64_t static_size;
    bool static_fs_active;
    bool static_fs_ref;
    bool static_streaming;
    bool static_headers_sent;
    bool static_cancel;
    bool static_head;

    /* io */
    bool reading;
    bool writing;
    bool closing;
    bool handler_running;
    bool timed_out;
    char *write_buf;
    size_t write_len;
    uv_write_t write_req;
    bool write_close_ref;
    char remote[64];
    int *res_refs;
    size_t nres_refs;
    size_t cres_refs;
    unsigned long req_generation;
    unsigned timer_mode;

    /* websocket */
    http_ws_t *ws;
};

/* util */
char *http_strdup(const char *s);
char *http_strndup(const char *s, size_t n);
void *http_realloc(void *p, size_t n);
void http_headers_free(http_header_t *h, size_t n);
bool http_headers_add(http_header_t **arr, size_t *n, size_t *cap,
                      const char *name, const char *value, bool replace);
const char *http_header_get(const http_header_t *h, size_t n, const char *name);
const char *http_status_reason(int status);
void http_date_now(char *buf, size_t n);
bool http_percent_decode(const char *in, size_t len, bool plus, str_t *out);
bool http_parse_urlencoded(lua_State *L, const char *data, size_t len,
                           int table_idx);
bool http_path_safe(const char *root, const char *rel, str_t *out);
char *http_sanitize_filename(const char *name);
bool http_mkdir_p(const char *path);
void http_log(const char *fmt, ...);

/* cookies */
bool http_cookies_parse(lua_State *L, const char *cookie, int table_idx);
bool http_cookie_header(http_conn_t *c, const char *name, const char *value,
                        int opts_idx, lua_State *L);

/* multipart */
bool http_multipart_parse(http_conn_t *c, lua_State *L, int form_idx,
                          int files_idx);

/* json */
bool http_json_encode(lua_State *L, int idx, str_t *out, size_t max_output,
                      int max_depth);
bool http_json_decode(lua_State *L, const char *s, size_t len, int max_depth,
                      str_t *err);

/* static files */
bool http_static_add(http_state_t *st, const char *prefix, const char *root,
                     str_t *err);
bool http_static_serve(http_conn_t *c, const char *method, const char *path);
void http_static_cancel(http_conn_t *c);
void http_static_cleanup(http_conn_t *c);
void http_statics_free(http_static_t *statics, size_t n);

/* router */
bool http_route_add(http_state_t *st, const char *method, const char *pattern,
                    int handler_ref, str_t *err);
http_route_t *http_route_match(http_state_t *st, const char *method,
                               const char *path);
bool http_route_match_path(http_route_t *r, const char *path);
void http_route_fill_params(http_route_t *r, const char *path, lua_State *L,
                            int table_idx);
bool http_route_allowed(http_state_t *st, const char *path, str_t *allow);
void http_routes_free(http_route_t *routes, size_t n);

/* request */
bool http_build_request(lua_State *L, http_conn_t *c, const char *path,
                        const char *query, const char *raw_path,
                        http_route_t *route);
int http_request_push_metatable(lua_State *L);

/* response */
int http_response_push(lua_State *L, http_conn_t *c);
void http_response_detach_all(http_conn_t *c);
void http_response_reset(http_conn_t *c);
bool http_conn_send_response(http_conn_t *c);
bool http_conn_send_stream_head(http_conn_t *c, uv_write_cb cb);
bool http_conn_write_raw(http_conn_t *c, char *buf, size_t len,
                         uv_write_cb cb);
void http_conn_write_cleanup(http_conn_t *c);
bool http_conn_write_release(http_conn_t *c);
bool http_conn_release_ref(http_conn_t *c);
void http_conn_send_error(http_conn_t *c, int status, const char *msg);

/* connection */
void http_conn_close(http_conn_t *c);
void http_conn_reset_request(http_conn_t *c);
void http_conn_write_cb(uv_write_t *req, int status);
void http_conn_timer_set(http_conn_t *c, unsigned mode, unsigned timeout_secs);

/* server */
http_state_t *http_state_get(lua_State *L);
int http_state_start(lua_State *L, http_state_t *st);
void http_state_stop(http_state_t *st);
void http_state_run(lua_State *L);
bool http_state_deinit(lua_State *L);

/* websocket */
bool http_ws_init(http_conn_t *c, lua_State *L, http_route_t *route,
                  int req_idx);
void http_ws_feed(http_conn_t *c, const char *data, size_t len);
void http_ws_disconnect(http_conn_t *c);
void http_ws_cleanup(http_conn_t *c);

/* module */
int luaopen_http(lua_State *L);
int http_dispatch(http_conn_t *c);

#endif /* TMG_HTTP_H */
