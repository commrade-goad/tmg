#include "http.h"
#include "str.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static lua_State *fuzz_L;

static void ensure_state(void) {
    if (!fuzz_L)
        fuzz_L = luaL_newstate();
}

__attribute__((destructor))
static void close_state(void) {
    if (fuzz_L) {
        lua_close(fuzz_L);
        fuzz_L = NULL;
    }
}

static char *copy_cstring(const uint8_t *data, size_t size) {
    char *s = malloc(size + 1);
    if (!s) abort();
    if (size)
        memcpy(s, data, size);
    s[size] = '\0';
    return s;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1u << 20))
        return 0;

    ensure_state();
    if (!fuzz_L)
        return 0;

    int base = lua_gettop(fuzz_L);

    str_t decoded = str_init((int)size + 1);
    http_percent_decode((const char *)data, size, size > 0 && (data[0] & 1),
                        &decoded);

    str_t safe = str_init((int)decoded.len + 1);
    http_path_safe(".", decoded.data, &safe);
    str_deinit(&safe);

    char *cstr = copy_cstring(data, size);
    char *filename = http_sanitize_filename(cstr);
    free(filename);

    lua_createtable(fuzz_L, 0, 0);
    http_parse_urlencoded(fuzz_L, (const char *)data, size, -1);
    lua_pop(fuzz_L, 1);

    lua_createtable(fuzz_L, 0, 0);
    http_cookies_parse(fuzz_L, cstr, -1);
    lua_pop(fuzz_L, 1);

    int depth = size > 0
        ? 1 + (int)(data[0] % (unsigned)HTTP_JSON_MAX_DEPTH)
        : HTTP_JSON_DEFAULT_DEPTH;
    str_t err = str_init(64);
    if (http_json_decode(fuzz_L, (const char *)data, size, depth, &err)) {
        str_t encoded = str_init(128);
        size_t budget = size > 4096 ? 16384 : size * 4 + 16;
        http_json_encode(fuzz_L, -1, &encoded, budget,
                         HTTP_JSON_DEFAULT_DEPTH);
        str_deinit(&encoded);
    }
    str_deinit(&err);

    lua_settop(fuzz_L, base);
    free(cstr);
    str_deinit(&decoded);
    return 0;
}
