#ifndef LEFTOVERS_H
#define LEFTOVERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scans internal storage for orphaned updates, DLCs, and metadata where the base package is missing.
 * Returns dynamically allocated JSON string (caller must free()), or NULL on error.
 */
char *leftovers_scan_json(void);

/**
 * Deletes all leftover paths for the specified title_id.
 * Returns dynamically allocated JSON string (caller must free()), or NULL on error.
 */
char *leftovers_delete_json(const char *title_id);


#ifdef __cplusplus
}
#endif

#endif /* LEFTOVERS_H */
