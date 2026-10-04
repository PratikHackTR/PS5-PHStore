#ifndef PHSTORE_UPSTREAM_H
#define PHSTORE_UPSTREAM_H

#include <stdint.h>
#include "phstore_install.h"
#include "phstore_raw_http.h"

typedef struct {
    int request_id;
    uint64_t requested_start;
    uint64_t requested_end;
    uint64_t total_size;
    uint64_t response_length;
    int transport_type;
    phstore_raw_http_response_t raw;
} phstore_upstream_response_t;

typedef struct {
    uint64_t bytes_received;
    uint32_t request_count;
    uint32_t head_count;
    uint32_t redirect_count;
    uint32_t read_failures;
    int status_code;
    int head_status_code;
    int range_probe_status_code;
    uint64_t head_content_length;
    int32_t native_result;
    char native_function[48];
    char scheme[8];
    char host[256];
    uint16_t port;
    char last_content_range[96];
    char stage[48];
} phstore_upstream_diagnostics_t;

int phstore_upstream_init(char error[64]);
void phstore_upstream_term(void);
int phstore_upstream_probe(const phstore_package_info_t *package, char error[64]);
int phstore_upstream_open_range(const phstore_package_info_t *package,
                                uint64_t start, uint64_t end,
                                phstore_upstream_response_t *response, char error[64]);
int phstore_upstream_read(phstore_upstream_response_t *response, void *buffer, size_t capacity);
void phstore_upstream_close(phstore_upstream_response_t *response);
void phstore_upstream_get_diagnostics(phstore_upstream_diagnostics_t *diagnostics);
void phstore_upstream_reset_diagnostics(const char *url);

#endif
