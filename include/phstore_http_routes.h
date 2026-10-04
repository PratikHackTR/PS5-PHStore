#ifndef PHSTORE_HTTP_ROUTES_H
#define PHSTORE_HTTP_ROUTES_H

#include <stddef.h>
#include <string.h>

typedef enum {
    PHSTORE_CATALOG_ROUTE_READY,
    PHSTORE_CATALOG_ROUTE_LOADING,
    PHSTORE_CATALOG_ROUTE_ERROR
} phstore_catalog_route_result_t;

static inline int phstore_request_path_matches(const char *method, const char *path,
                                               const char *wanted_method, const char *wanted_path) {
    if (!method || !path || strcmp(method, wanted_method) != 0) return 0;
    size_t path_length = strcspn(path, "?");
    size_t wanted_length = strlen(wanted_path);
    while (path_length > 1 && path[path_length - 1] == '/') path_length--;
    while (wanted_length > 1 && wanted_path[wanted_length - 1] == '/') wanted_length--;
    return path_length == wanted_length && memcmp(path, wanted_path, wanted_length) == 0;
}

static inline phstore_catalog_route_result_t phstore_catalog_route_result(int catalog_available,
                                                                          int catalog_failed) {
    if (catalog_available) return PHSTORE_CATALOG_ROUTE_READY;
    return catalog_failed ? PHSTORE_CATALOG_ROUTE_ERROR : PHSTORE_CATALOG_ROUTE_LOADING;
}

#endif
