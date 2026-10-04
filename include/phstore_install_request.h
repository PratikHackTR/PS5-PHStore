#ifndef PHSTORE_INSTALL_REQUEST_H
#define PHSTORE_INSTALL_REQUEST_H

#include "phstore_install.h"

#include <stddef.h>

int phstore_install_request_parse(const char *json, size_t length,
                                  char package_id[PHSTORE_PACKAGE_ID_MAX]);

#endif
