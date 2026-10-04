#ifndef PHSTORE_RAW_HTTP_H
#define PHSTORE_RAW_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include "phstore_install.h"

typedef struct {
    int fd;
    uint64_t remaining;
    unsigned char buffered[16384];
    size_t buffered_length;
    size_t buffered_offset;
    uint64_t connect_ms;
    uint64_t header_wait_ms;
    uint64_t body_ms;
    uint64_t body_started_at_ms;
    uint64_t bytes_received;
    uint64_t first_byte_at_ms;
    uint8_t header_cache_hit;
    uint8_t cache_backed;
    uint64_t cache_offset;
    char endpoint_host[256];
    char endpoint_port[8];
} phstore_raw_http_response_t;

int phstore_raw_http_probe(const phstore_package_info_t *package, char error[64]);
void phstore_raw_http_source_begin(const phstore_package_info_t *package);
void phstore_raw_http_source_end(void);
int phstore_raw_http_open_range(const phstore_package_info_t *package, uint64_t start,
                                uint64_t end, phstore_raw_http_response_t *response,
                                char error[64]);
int phstore_raw_http_read(phstore_raw_http_response_t *response, void *buffer, size_t capacity);
void phstore_raw_http_close(phstore_raw_http_response_t *response);
void phstore_raw_http_cancel(void);
void phstore_raw_http_get_diagnostics(uint64_t *bytes, uint32_t *requests, uint32_t *heads,
                                      uint32_t *redirects, uint32_t *read_failures,
                                      int *status, int *head_status, uint64_t *head_size,
                                      int *range_probe_status, char stage[48], char last_content_range[96],
                                      int32_t *native_result);
void phstore_raw_http_reset_diagnostics(void);
void phstore_raw_http_get_cache_diagnostics(uint64_t *hits, uint64_t *bytes_served,
                                            uint64_t *misses);

#endif
