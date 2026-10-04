#ifndef PHSTORE_RAW_IMAGE_HTTP_H
#define PHSTORE_RAW_IMAGE_HTTP_H
#include <stddef.h>
#include <stdint.h>

/* Cover-only transport. No package diagnostics, cancellation or header cache. */
#define PHSTORE_COVER_HOST "148.135.181.3"
#define PHSTORE_COVER_PATH "/phstore/covers/"
#define PHSTORE_COVER_BASE_URL "http://" PHSTORE_COVER_HOST PHSTORE_COVER_PATH
typedef struct {
    int fd, status;
    uint64_t content_length, remaining;
    size_t buffered_length, buffered_offset;
    unsigned char buffered[16385];
    const char *stage;
} phstore_raw_image_response_t;

/* Fixed HTTP origin, 32-hex key, GET 200 with bounded Content-Length only.
 * No redirects or fallback to the original cover URL. errno on failure;
 * response status/stage are retained, including HTTP 404. */
int phstore_raw_image_open(const char *key, uint64_t maximum, phstore_raw_image_response_t *response);
int phstore_raw_image_read(phstore_raw_image_response_t *response, void *buffer, size_t capacity);
void phstore_raw_image_close(phstore_raw_image_response_t *response);
#endif
