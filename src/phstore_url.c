#include "phstore_url.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int ascii_equal(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

int phstore_url_validate(const char *url) {
    if (!url) return 0;
    size_t n = strlen(url);
    if (n < 8 || n >= 2048) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '#') return 0;
    }
    size_t start;
    if (n >= 7 && ascii_equal(url, "http://", 7)) start = 7;
    else if (n >= 8 && ascii_equal(url, "https://", 8)) start = 8;
    else return 0;
    size_t end = start;
    while (end < n && url[end] != '/' && url[end] != '?') end++;
    if (end == start || memchr(url + start, '@', end - start)) return 0;
    size_t port_start = end;
    if (url[start] == '[') {
        size_t close = start + 1;
        while (close < end && url[close] != ']') {
            if (!isxdigit((unsigned char)url[close]) && url[close] != ':' && url[close] != '.') return 0;
            close++;
        }
        if (close == start + 1 || close >= end) return 0;
        if (close + 1 < end) {
            if (url[close + 1] != ':') return 0;
            port_start = close + 2;
        } else port_start = end;
    } else {
        size_t colon = end;
        for (size_t i = start; i < end; i++) {
            if (url[i] == ':') { colon = i; break; }
            if (!isalnum((unsigned char)url[i]) && url[i] != '.' && url[i] != '-') return 0;
        }
        if (colon == start) return 0;
        if (colon < end) port_start = colon + 1;
    }
    if (port_start < end) {
        unsigned port = 0;
        for (size_t i = port_start; i < end; i++) {
            if (url[i] < '0' || url[i] > '9' || port > 6553) return 0;
            port = port * 10 + (unsigned)(url[i] - '0');
        }
        if (!port || port > 65535) return 0;
    } else if (port_start == end && end > start && url[end - 1] == ':') return 0;
    return 1;
}

static int copy_part(char *output, size_t capacity, size_t *used, const char *part, size_t length) {
    if (*used > capacity || length >= capacity - *used) return 0;
    memcpy(output + *used, part, length);
    *used += length;
    output[*used] = '\0';
    return 1;
}

int phstore_url_resolve_redirect(const char *base_url, const char *location,
                                 char *output, size_t output_capacity) {
    if (!base_url || !location || !output || !output_capacity) return 0;
    output[0] = '\0';
    size_t location_length = strcspn(location, "#");
    if (!location_length || location_length >= 2048) return 0;
    char target[2048];
    memcpy(target, location, location_length); target[location_length] = '\0';
    if (strstr(target, "://")) {
        if (!phstore_url_validate(target) || strlen(target) >= output_capacity) return 0;
        memcpy(output, target, strlen(target) + 1);
        return 1;
    }

    size_t base_length = strlen(base_url);
    size_t scheme_length = ascii_equal(base_url, "https://", 8) ? 8 :
                           ascii_equal(base_url, "http://", 7) ? 7 : 0;
    if (!scheme_length || base_length < scheme_length) return 0;
    size_t authority_end = scheme_length;
    while (authority_end < base_length && base_url[authority_end] != '/' &&
           base_url[authority_end] != '?' && base_url[authority_end] != '#') authority_end++;
    size_t used = 0;
    if (target[0] == '/' && target[1] == '/') {
        if (!copy_part(output, output_capacity, &used, base_url, scheme_length) ||
            !copy_part(output, output_capacity, &used, target, location_length)) return 0;
    } else {
        if (!copy_part(output, output_capacity, &used, base_url, authority_end)) return 0;
        if (target[0] == '/') {
            if (!copy_part(output, output_capacity, &used, target, location_length)) return 0;
        } else if (target[0] == '?') {
            size_t path_end = authority_end;
            while (path_end < base_length && base_url[path_end] != '?' && base_url[path_end] != '#') path_end++;
            if (path_end == authority_end && !copy_part(output, output_capacity, &used, "/", 1)) return 0;
            else if (path_end > authority_end && !copy_part(output, output_capacity, &used,
                    base_url + authority_end, path_end - authority_end)) return 0;
            if (!copy_part(output, output_capacity, &used, target, location_length)) return 0;
        } else {
            size_t path_start = authority_end;
            size_t path_end = path_start;
            while (path_end < base_length && base_url[path_end] != '?' && base_url[path_end] != '#') path_end++;
            size_t directory_end = path_end;
            while (directory_end > path_start && base_url[directory_end - 1] != '/') directory_end--;
            if (directory_end > path_start) {
                if (!copy_part(output, output_capacity, &used, base_url + path_start, directory_end - path_start)) return 0;
            } else if (!copy_part(output, output_capacity, &used, "/", 1)) return 0;
            if (!copy_part(output, output_capacity, &used, target, location_length)) return 0;
        }
    }
    return phstore_url_validate(output);
}
