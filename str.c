#include "str.h"

str_t str_init(int cap) {
    if (cap == -1)
        cap = 10;
    str_t a = (str_t){
        .data = malloc(sizeof(char) * cap),
        .len = 0,
        .cap = cap,
    };
    a.data[0] = '\0';
    return a;
}

void str_deinit(str_t *str) {
    if (str && str->data) {
        free(str->data);
        str->data = NULL;
        str->len = 0;
        str->cap = 0;
    }
}

void str_push_chr(str_t *str, const char data) {
    if (str->len + 2 >= str->cap) {
        str->cap *= 2;
        str->data = realloc(str->data, sizeof(char) * str->cap);
    }
    str->data[str->len] = data;
    str->len += 1;
    str->data[str->len] = '\0';
}

void str_pop_chr(str_t *str) {
    if (str->len > 0) {
        str->len -= 1;
        str->data[str->len] = '\0';
    }
}

void str_clear(str_t *str) {
    str->len = 0;
    if (str->data) {
        str->data[0] = '\0';
    }
}

void str_push(str_t *str, const char *data) {
    size_t len = strlen(data);
    if (str->len + len + 1 >= str->cap) {
        str->cap = (str->len + len + 1) * 2;
        str->data = realloc(str->data, sizeof(char) * str->cap);
    }
    memcpy(str->data + str->len, data, len);
    str->len += len;
    str->data[str->len] = '\0';
}

arrstr_t arrstr_init(int cap) {
    if (cap <= 0) {
        cap = 10;
    }
    arrstr_t a = {0};
    a = (arrstr_t){.len = 0, .cap = cap, .data = malloc(sizeof(str_t) * cap)};
    return a;
}

void arrstr_deinit(arrstr_t *astr) {
    for (size_t i = 0; i < astr->len; i++) {
        str_deinit(&astr->data[i]);
    }
    free(astr->data);
}

void arrstr_push(arrstr_t *astr, const char *c) {
    if (!astr || !c) return;
    if (astr->len >= astr->cap) {
        astr->cap *= 2;
        astr->data = realloc(astr->data, sizeof(str_t) * astr->cap);
    }

    str_t new_str = str_init(-1); // Initialize with default capacity
    str_push(&new_str, c);        // Copy the string content

    astr->data[astr->len] = new_str;
    astr->len++;
}
