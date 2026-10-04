#ifndef APP_INFO_H
#define APP_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Global SQLite serialization mutex (THREADSAFE=2 still needs app-level
 * protection when MHD threads + installer thread share app.db). */
extern pthread_mutex_t g_appinfo_db_mutex;

/**
 * Checks if a base application is installed on the console.
 * title_id: The Title ID (e.g. "CUSA12345", "PPSA12345").
 * out_version: Buffer to receive the installed version (e.g. "v1.03", "v01.06"). Can be NULL.
 * max_ver_len: Size of out_version buffer.
 *
 * Checks:
 * 1. /system_data/priv/mms/app.db (or PKG_APP_DB_PATH) via SQLite3
 * 2. /system_data/priv/appmeta/<title_id>/param.json or param.sfo (or PKG_APPMETA_DIR)
 * 3. /system_data/priv/appmeta/external/<title_id>/...
 * 4. /user/app/<title_id>/sce_sys/param.json or param.sfo (or PKG_USER_APP_DIR)
 * 5. /system_ex/app/<title_id>/sce_sys/param.json
 * 6. sceAppInstUtilAppExists(title_id) on PS5
 *
 * Returns 1 if installed, 0 if not installed.
 */
int app_info_check_installed(const char *title_id, char *out_version, size_t max_ver_len);

/**
 * Checks if a specific DLC (Additional Content) is installed for the given title.
 * title_id: The Title ID (e.g. "CUSA12346").
 * content_id: The DLC Content ID (e.g. "UP1234-CUSA12346_00-TESTPACK0000008").
 *
 * Checks:
 * 1. /system_data/priv/mms/app.db (or PKG_APP_DB_PATH) via SQLite3
 * 2. /user/addcont/<title_id>/... (or PKG_ADDCONT_DIR)
 * 3. /system_data/priv/addcontmeta/<title_id>/<content_id>
 * 4. /system_data/priv/appmeta/<title_id>/addcont/<content_id>
 * 5. sceAppInstUtilGetAddcontInstalledStatus on PS5
 *
 * Returns 1 if installed, 0 if not installed.
 */
int app_info_check_dlc_installed(const char *title_id, const char *content_id);

/**
 * Checks if a title has orphaned/leftover updates, DLCs, or metadata while the base package is missing.
 * Returns 1 if leftovers exist and base is missing, 0 otherwise.
 * If out_desc is non-NULL, writes a brief description (e.g. "Orphaned update v01.26").
 */
int app_info_check_has_leftover(const char *title_id, char *out_desc, size_t max_desc_len);

/**
 * Checks if an application has an incomplete or aborted installation on the console
 * (e.g. aborted multi-part or stream install leaving a broken home screen tile with contentStatus=1 or size=0).
 * Returns 1 if partially installed / aborted, 0 otherwise.
 * If out_desc is non-NULL, writes a brief description.
 */
int app_info_check_partially_installed(const char *title_id, char *out_desc, size_t max_desc_len);

/**
 * Compares two version strings (e.g. "v1.00", "v01.06", "1.03", "01.000.003").
 * Returns:
 *   < 0 if ver_a < ver_b
 *   0   if ver_a == ver_b
 *   > 0 if ver_a > ver_b
 */
int app_info_compare_versions(const char *ver_a, const char *ver_b);

/**
 * Normalizes a version string (e.g. "01.000.003" -> "v1.03", "01.06" -> "v01.06", "06.000.000" -> "v6.00").
 */
void app_info_normalize_version(const char *in, char *out, size_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* APP_INFO_H */
