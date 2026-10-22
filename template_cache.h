#ifndef TMG_TEMPLATE_CACHE_H
#define TMG_TEMPLATE_CACHE_H

#include <stdbool.h>
#include <stddef.h>

#include <lua.h>

#include "str.h"

typedef struct tmg_template_cache tmg_template_cache_t;

enum {
    TMG_CACHE_OFF = 0,
    TMG_CACHE_DEVELOPMENT = 1,
    TMG_CACHE_PRODUCTION = 2
};

tmg_template_cache_t *tmg_template_cache_create(int mode, size_t max_entries);
void tmg_template_cache_set(tmg_template_cache_t *cache, lua_State *L,
                            int mode, size_t max_entries);
void tmg_template_cache_clear(tmg_template_cache_t *cache, lua_State *L);
void tmg_template_cache_free(tmg_template_cache_t *cache, lua_State *L);
int tmg_template_cache_stats(lua_State *L, tmg_template_cache_t *cache);

bool tmg_render_cached(lua_State *L, tmg_template_cache_t *cache,
                       const char *in_path, const char *sep, bool debug,
                       int data_idx, bool html, size_t max_output,
                       str_t *err);

#endif
