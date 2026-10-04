#include "phstore_install_request.h"

#include <ctype.h>
#include <string.h>

static void skip_space(const char **cursor, const char *end) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t' ||
           **cursor == '\r' || **cursor == '\n')) (*cursor)++;
}

int phstore_install_request_parse(const char *json, size_t length,
                                  char package_id[PHSTORE_PACKAGE_ID_MAX]) {
    if (!json || !package_id) return 0;
    const char *cursor = json, *end = json + length;
    skip_space(&cursor, end);
    if (cursor >= end || *cursor++ != '{') return 0;
    skip_space(&cursor, end);
    static const char key[] = "\"package_id\"";
    if ((size_t)(end - cursor) < sizeof(key) - 1 || memcmp(cursor, key, sizeof(key) - 1)) return 0;
    cursor += sizeof(key) - 1;
    skip_space(&cursor, end);
    if (cursor >= end || *cursor++ != ':') return 0;
    skip_space(&cursor, end);
    if (cursor >= end || *cursor++ != '"') return 0;
    size_t used = 0;
    while (cursor < end && *cursor != '"') {
        unsigned char c = (unsigned char)*cursor++;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.') ||
            used + 1 >= PHSTORE_PACKAGE_ID_MAX) return 0;
        package_id[used++] = (char)c;
    }
    if (!used || cursor >= end || *cursor++ != '"') return 0;
    package_id[used] = '\0';
    skip_space(&cursor, end);
    if (cursor >= end || *cursor++ != '}') return 0;
    skip_space(&cursor, end);
    return cursor == end;
}
