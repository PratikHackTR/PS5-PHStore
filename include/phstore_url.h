#ifndef PHSTORE_URL_H
#define PHSTORE_URL_H

#include <stddef.h>

int phstore_url_validate(const char *url);
int phstore_url_resolve_redirect(const char *base_url, const char *location,
                                 char *output, size_t output_capacity);

#endif
