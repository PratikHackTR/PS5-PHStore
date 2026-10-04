#include "phstore_range.h"

#include <stdint.h>
#include <string.h>

static int parse_decimal(const char *text, const char **end, uint64_t *value) {
    if (!text || *text < '0' || *text > '9') return 0;
    uint64_t result = 0;
    const char *cursor = text;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned digit = (unsigned)(*cursor - '0');
        if (result > (UINT64_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
        cursor++;
    }
    if (end) *end = cursor;
    *value = result;
    return 1;
}

int phstore_range_parse(const char *value, uint64_t total, uint64_t *start, uint64_t *end) {
    if (!value || !*value) return 0;
    if (!start || !end || strncmp(value, "bytes=", 6) != 0 || strchr(value, ',')) return -1;
    const char *first = value + 6;
    const char *dash = strchr(first, '-');
    if (!dash || !total) return -1;
    uint64_t a = 0, b = 0;
    const char *tail;
    if (dash == first) {
        if (!parse_decimal(dash + 1, &tail, &b) || *tail || !b) return -1;
        if (b > total) b = total;
        a = total - b;
        b = total - 1;
    } else {
        if (!parse_decimal(first, &tail, &a) || tail != dash) return -1;
        if (dash[1]) {
            if (!parse_decimal(dash + 1, &tail, &b) || *tail || b < a) return -1;
            if (b >= total) b = total - 1;
        } else b = total - 1;
        if (a >= total) return -1;
    }
    *start = a; *end = b;
    return 1;
}

static int parse_content_u64(const char **cursor, char delimiter, uint64_t *value) {
    const char *p = *cursor;
    if (*p < '0' || *p > '9') return 0;
    uint64_t result = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
        p++;
    }
    if (*p != delimiter) return 0;
    *cursor = p + 1;
    *value = result;
    return 1;
}

static int content_range_parse(const char *text, uint64_t *start,
                               uint64_t *end, uint64_t *total) {
    if (!text || strncmp(text, "bytes ", 6) != 0) return 0;
    const char *cursor = text + 6;
    if (!parse_content_u64(&cursor, '-', start) || !parse_content_u64(&cursor, '/', end)) return 0;
    if (*cursor < '0' || *cursor > '9') return 0;
    uint64_t parsed_total = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned digit = (unsigned)(*cursor - '0');
        if (parsed_total > (UINT64_MAX - digit) / 10) return 0;
        parsed_total = parsed_total * 10 + digit;
        cursor++;
    }
    if (*cursor != '\0' || *start > *end || *end >= parsed_total) return 0;
    *total = parsed_total;
    return 1;
}

int phstore_content_range_validate(int status, const char *content_range,
                                   uint64_t content_length, uint64_t expected_start,
                                   uint64_t expected_end, uint64_t expected_total) {
    uint64_t start, end, total;
    if (status != 206 || expected_start > expected_end || expected_end >= expected_total ||
        !content_range_parse(content_range, &start, &end, &total) ||
        start != expected_start || end != expected_end || total != expected_total) return 0;
    return content_length == expected_end - expected_start + 1;
}
