#ifndef PHSTORE_SOURCE_MAP_H
#define PHSTORE_SOURCE_MAP_H

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *scheme;
    const char *host;
    unsigned short port;
    const char *base_path;
} phstore_source_group_t;

static const phstore_source_group_t PHSTORE_SOURCE_GROUPS[] = {
    {"PS2Games", "http", "148.135.181.3", 80, "/ps2/"},
    {"PS4Games", "http", "148.135.181.3", 80, "/ps4/"},
    {"PS5Games", "http", "148.135.181.3", 80, "/ps5/"}
};

#define PHSTORE_SOURCE_GROUP_COUNT (sizeof(PHSTORE_SOURCE_GROUPS) / sizeof(PHSTORE_SOURCE_GROUPS[0]))

static inline int phstore_source_group_resolve(const char *group, const char *filename,
                                               char *url, size_t capacity) {
    if (!group || !filename || !url || !capacity) return -1;
    const phstore_source_group_t *mapping = NULL;
    for (size_t i = 0; i < PHSTORE_SOURCE_GROUP_COUNT; i++)
        if (strcmp(group, PHSTORE_SOURCE_GROUPS[i].name) == 0) { mapping = &PHSTORE_SOURCE_GROUPS[i]; break; }
    if (!mapping) return -2;
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    int head = snprintf(url, capacity, "%s://%s%s", mapping->scheme, mapping->host, mapping->base_path);
    if (head <= 0 || (size_t)head >= capacity) return -1;
    used = (size_t)head;
    for (const unsigned char *c = (const unsigned char *)filename; *c; c++) {
        if (isalnum(*c) || *c == '-' || *c == '_' || *c == '.' || *c == '~') {
            if (used + 1 >= capacity) return -1;
            url[used++] = (char)*c;
        } else {
            if (used + 3 >= capacity) return -1;
            url[used++] = '%'; url[used++] = hex[*c >> 4]; url[used++] = hex[*c & 15];
        }
    }
    url[used] = '\0';
    return 0;
}

#endif
