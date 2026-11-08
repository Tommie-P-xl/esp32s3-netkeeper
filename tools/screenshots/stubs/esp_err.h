#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
static inline size_t docs_strlcpy(char *out, const char *in, size_t cap)
{
    size_t n = strlen(in);
    if (cap) {
        size_t count = n < cap - 1 ? n : cap - 1;
        memcpy(out, in, count);
        out[count] = 0;
    }
    return n;
}
#define strlcpy docs_strlcpy
