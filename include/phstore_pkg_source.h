#ifndef PHSTORE_PKG_SOURCE_H
#define PHSTORE_PKG_SOURCE_H

#include <stddef.h>
#include <stdint.h>

/* Adapter seam called by the vendored pkg-manager virtual_stream layer. */
int phstore_pkg_source_open_uri(const char *uri, void **handle,
                               uint64_t *size, char *filename,
                               size_t filename_capacity);
int64_t phstore_pkg_source_read_at(void *handle, uint64_t offset,
                                  void *buffer, size_t length);
void phstore_pkg_source_close(void *handle);

#endif
