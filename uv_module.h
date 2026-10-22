#ifndef TMG_UV_MODULE_H
#define TMG_UV_MODULE_H

#include <lua.h>

int luaopen_uv(lua_State *L);
void tmg_uv_shutdown(lua_State *L);

void tmg_uv_fs_register(lua_State *L);
void tmg_uv_fs_shutdown(lua_State *L);

/*
 * TODO: libuv surface not exposed yet.
 *
 *   udp          uv_udp_t bind/send/recv/connect
 *   pipe         uv_pipe_t IPC and stdio streams
 *   tty          uv_tty_t
 *   process      uv_spawn / uv_process_t, stdio inheritance
 *   dns          uv_getaddrinfo / uv_getnameinfo
 *   signals      uv_signal_t (HTTP owns SIGINT/SIGTERM internally)
 *   fs_event     uv_fs_event_t (inotify/kqueue/FSEvents)
 *   fs_poll      uv_fs_poll_t
 *   random       uv_random
 *   threadpool   uv_queue_work / uv_work_t; keep Lua callbacks on loop thread
 *   misc         uv_uptime, uv_loadavg, uv_interface_addresses,
 *                uv_cpu_info, uv_os_uname, uv_getrusage
 */

#endif
