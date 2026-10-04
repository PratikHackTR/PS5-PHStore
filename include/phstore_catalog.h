#ifndef PHSTORE_CATALOG_H
#define PHSTORE_CATALOG_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PHSTORE_CATALOG_LOADING,
    PHSTORE_CATALOG_READY,
    PHSTORE_CATALOG_ERROR,
    PHSTORE_CATALOG_REFRESHING
} phstore_catalog_state_t;

typedef struct {
    phstore_catalog_state_t state;
    int has_catalog;
    uint32_t schema_version;
    uint32_t catalog_version;
    size_t game_count;
    char generated_at[64];
    char last_error[64];
} phstore_catalog_status_t;

void phstore_catalog_init(void);
int phstore_catalog_start(void);
/* 0 started, 1 already loading/refreshing, -1 thread creation failed. */
int phstore_catalog_refresh(void);
void phstore_catalog_get_status(phstore_catalog_status_t *status);
/* Returns an owned NUL-terminated snapshot, or NULL if no valid catalog exists. */
char *phstore_catalog_copy(size_t *length);
char *phstore_catalog_game_copy(const char *id, size_t *length);

/* Owned, deduplicated cover URL snapshot; free each URL then the array. */
char **phstore_catalog_cover_urls(size_t *count);

#endif
