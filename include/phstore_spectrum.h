#ifndef PHSTORE_SPECTRUM_H
#define PHSTORE_SPECTRUM_H
#include <stddef.h>
int phstore_spectrum_active(void);
const char *phstore_spectrum_start_json(const char *body, size_t length);
const char *phstore_spectrum_install_start_json(const char *body, size_t length);
int phstore_spectrum_pause(void);
int phstore_spectrum_resume(void);
int phstore_spectrum_cancel_delete(void);
size_t phstore_spectrum_status_json(char *body, size_t capacity);
#endif
