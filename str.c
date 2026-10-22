#include "str.h"

#include <stdint.h>
#include <stdio.h>

static void str_oom(void) {
    fprintf(stderr, "ERR: out of memory\n");
    abort();
}

str_t str_init(int cap) {
    if (cap <= 0)
        cap = 16;
    str_t a = {0};
    a.data = malloc((size_t)cap);
    if (!a.data)
        str_oom();
    a.len = 0;
    a.cap = (size_t)cap;
    a.data[0] = '\0';
    return a;
}

void str_deinit(str_t *str) {
    if (!str) return;
    free(str->data);
    str->data = NULL;
    str->len = 0;
    str->cap = 0;
}

bool str_reserve(str_t *str, size_t need) {
    if (!str) return false;
    if (need <= str->cap) return true;

    size_t cap = str->cap ? str->cap : 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    char *p = realloc(str->data, cap);
    if (!p) return false;
    str->data = p;
    str->cap = cap;
    return true;
}

void str_push_chr(str_t *str, const char data) {
    if (!str_reserve(str, str->len + 2))
        str_oom();
    str->data[str->len] = data;
    str->len += 1;
    str->data[str->len] = '\0';
}

void str_push_len(str_t *str, const char *data, size_t len) {
    if (len == 0) return;
    if (!data) return;
    if (len > SIZE_MAX - str->len - 1)
        str_oom();
    if (!str_reserve(str, str->len + len + 1))
        str_oom();
    memcpy(str->data + str->len, data, len);
    str->len += len;
    str->data[str->len] = '\0';
}

void str_pop_chr(str_t *str) {
    if (str && str->len > 0) {
        str->len -= 1;
        str->data[str->len] = '\0';
    }
}

void str_clear(str_t *str) {
    if (!str) return;
    str->len = 0;
    if (str->data) {
        str->data[0] = '\0';
    }
}

void str_push(str_t *str, const char *data) {
    if (!data) return;
    str_push_len(str, data, strlen(data));
}

arrstr_t arrstr_init(int cap) {
    if (cap <= 0)
        cap = 10;
    arrstr_t a = {0};
    a.cap = (size_t)cap;
    a.data = malloc(sizeof(str_t) * a.cap);
    if (!a.data)
        str_oom();
    a.len = 0;
    return a;
}

void arrstr_deinit(arrstr_t *astr) {
    if (!astr) return;
    for (size_t i = 0; i < astr->len; i++) {
        str_deinit(&astr->data[i]);
    }
    free(astr->data);
    astr->data = NULL;
    astr->len = 0;
    astr->cap = 0;
}

void arrstr_push(arrstr_t *astr, const char *c) {
    if (!astr || !c) return;
    if (astr->len >= astr->cap) {
        size_t cap = astr->cap ? astr->cap * 2 : 10;
        if (cap <= astr->cap)
            str_oom();
        str_t *p = realloc(astr->data, sizeof(str_t) * cap);
        if (!p)
            str_oom();
        astr->data = p;
        astr->cap = cap;
    }

    str_t new_str = str_init(-1);
    str_push(&new_str, c);

    astr->data[astr->len] = new_str;
    astr->len++;
}
