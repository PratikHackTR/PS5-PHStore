#ifndef PHSTORE_SHORTCUT_H
#define PHSTORE_SHORTCUT_H

#include <stdint.h>

int phstore_shortcut_install_if_needed(uint32_t *error_code);
int phstore_shortcut_force_install(uint32_t *error_code);
int phstore_shortcut_is_current(void);

#endif
