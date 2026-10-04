#ifndef PHSTORE_IMAGE_CACHE_H
#define PHSTORE_IMAGE_CACHE_H
#include <stddef.h>
/* Start one background worker; refresh requests coalesce into one rerun. */
int phstore_image_cache_start(int automatic);
char *phstore_image_cache_status_json(void);
char *phstore_image_cache_log_text(size_t *length);
char *phstore_image_cache_index_json(void);
/* Strict 32-hex image key; returns owned image bytes and static MIME type. */
unsigned char *phstore_image_cache_read(const char *key, size_t *length, const char **mime);
#endif
