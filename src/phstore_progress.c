#include "phstore_progress.h"

#include <stdlib.h>
#include <string.h>

#define PHSTORE_MAX_COVERAGE_INTERVALS 65536u

int phstore_coverage_init(phstore_coverage_t *coverage, uint64_t total_bytes) {
    if (!coverage || !total_bytes) return -1;
    memset(coverage, 0, sizeof(*coverage));
    coverage->total_bytes = total_bytes;
    coverage->maximum = PHSTORE_MAX_COVERAGE_INTERVALS;
    return 0;
}

void phstore_coverage_destroy(phstore_coverage_t *coverage) {
    if (!coverage) return;
    free(coverage->items);
    memset(coverage, 0, sizeof(*coverage));
}

int phstore_coverage_add(phstore_coverage_t *coverage, uint64_t start, uint64_t length) {
    if (!coverage || !coverage->total_bytes || !length || start >= coverage->total_bytes ||
        length > coverage->total_bytes - start) return -1;
    uint64_t end = start + length;
    size_t first = 0;
    while (first < coverage->count && coverage->items[first].end < start) first++;
    size_t after = first;
    uint64_t merged_start = start, merged_end = end, removed = 0;
    while (after < coverage->count && coverage->items[after].start <= merged_end) {
        if (coverage->items[after].start < merged_start) merged_start = coverage->items[after].start;
        if (coverage->items[after].end > merged_end) merged_end = coverage->items[after].end;
        removed += coverage->items[after].end - coverage->items[after].start;
        after++;
    }
    if (after == first && coverage->count == coverage->maximum) {
        coverage->exhausted = 1;
        return -2;
    }
    if (after == first && coverage->count == coverage->capacity) {
        size_t next = coverage->capacity ? coverage->capacity * 2 : 64;
        if (next > coverage->maximum) next = coverage->maximum;
        phstore_covered_interval_t *items = realloc(coverage->items, next * sizeof(*items));
        if (!items) { coverage->exhausted = 1; return -2; }
        coverage->items = items;
        coverage->capacity = next;
    }
    if (after == first) {
        memmove(&coverage->items[first + 1], &coverage->items[first],
                (coverage->count - first) * sizeof(*coverage->items));
        coverage->count++;
    } else {
        memmove(&coverage->items[first + 1], &coverage->items[after],
                (coverage->count - after) * sizeof(*coverage->items));
        coverage->count -= after - first - 1;
    }
    coverage->items[first].start = merged_start;
    coverage->items[first].end = merged_end;
    coverage->covered_bytes += (merged_end - merged_start) - removed;
    if (coverage->covered_bytes > coverage->total_bytes) coverage->covered_bytes = coverage->total_bytes;
    return 0;
}

uint64_t phstore_coverage_percent_x100(const phstore_coverage_t *coverage) {
    if (!coverage || !coverage->total_bytes) return 0;
    uint64_t value = coverage->covered_bytes > coverage->total_bytes ? coverage->total_bytes : coverage->covered_bytes;
    return (uint64_t)(((long double)value * 10000.0L) / (long double)coverage->total_bytes);
}

double phstore_speed_ema(double previous, uint64_t byte_delta, double seconds) {
    if (seconds <= 0.0) return previous;
    double instant = (double)byte_delta / seconds;
    return previous <= 0.0 ? instant : previous * 0.7 + instant * 0.3;
}

int phstore_eta_seconds(uint64_t remaining_bytes, uint64_t speed_bps, double *seconds) {
    if (!seconds) return 0;
    if (!speed_bps) { *seconds = 0.0; return 0; }
    *seconds = (double)remaining_bytes / (double)speed_bps;
    return 1;
}
