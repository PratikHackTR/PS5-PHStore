#ifndef PHSTORE_RANGE_H
#define PHSTORE_RANGE_H

#include <stdint.h>

/* 1 parsed, 0 absent, -1 malformed or unsatisfiable. end is inclusive. */
int phstore_range_parse(const char *value, uint64_t total, uint64_t *start, uint64_t *end);
/* Validates the native HTTP range response tuple, including 206 and exact body length. */
int phstore_content_range_validate(int status, const char *content_range,
                                   uint64_t content_length, uint64_t expected_start,
                                   uint64_t expected_end, uint64_t expected_total);

#endif
