#ifndef PHSTORE_PROGRESS_H
#define PHSTORE_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t start;
    uint64_t end;
} phstore_covered_interval_t;

typedef struct {
    phstore_covered_interval_t *items;
    size_t count;
    size_t capacity;
    size_t maximum;
    uint64_t total_bytes;
    uint64_t covered_bytes;
    int exhausted;
} phstore_coverage_t;

int phstore_coverage_init(phstore_coverage_t *coverage, uint64_t total_bytes);
void phstore_coverage_destroy(phstore_coverage_t *coverage);
int phstore_coverage_add(phstore_coverage_t *coverage, uint64_t start, uint64_t length);
uint64_t phstore_coverage_percent_x100(const phstore_coverage_t *coverage);
double phstore_speed_ema(double previous, uint64_t byte_delta, double seconds);
int phstore_eta_seconds(uint64_t remaining_bytes, uint64_t speed_bps, double *seconds);

#endif
