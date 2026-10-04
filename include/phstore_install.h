#ifndef PHSTORE_INSTALL_H
#define PHSTORE_INSTALL_H

#include <stddef.h>
#include <stdint.h>
#include "phstore_progress.h"

#define PHSTORE_PACKAGE_ID_MAX 96
#define PHSTORE_PACKAGE_FILENAME_MAX 256
#define PHSTORE_SOURCE_GROUP_MAX 64
#define PHSTORE_SOURCE_TYPE_MAX 32
#define PHSTORE_DOWNLOAD_URL_MAX 2048
#define PHSTORE_INSTALL_NAME_MAX 256

typedef struct {
    char package_id[PHSTORE_PACKAGE_ID_MAX];
    char game_id[PHSTORE_PACKAGE_ID_MAX];
    char game_title[PHSTORE_INSTALL_NAME_MAX];
    char title_id[16];
    char package_type[24];
    char filename[PHSTORE_PACKAGE_FILENAME_MAX];
    char version[32];
    char platform[8];
    char source_group[PHSTORE_SOURCE_GROUP_MAX];
    char source_type[PHSTORE_SOURCE_TYPE_MAX];
    char download_url[PHSTORE_DOWNLOAD_URL_MAX];
    /* Optional native public Drive file download. Legacy records stay unchanged. */
    char action_type[32];
    char file_id[129];
    char sha256[65];
    uint64_t size_bytes;
    int installable;
} phstore_package_info_t;

typedef enum {
    PHSTORE_INSTALL_IDLE,
    PHSTORE_INSTALL_STARTING,
    PHSTORE_INSTALL_CANCELING,
    PHSTORE_INSTALL_INSTALLING,
    PHSTORE_INSTALL_COMPLETED,
    PHSTORE_INSTALL_FAILED,
    PHSTORE_INSTALL_CANCELLED,
    PHSTORE_INSTALL_DIAGNOSTIC_OK,
    PHSTORE_INSTALL_DIAGNOSTIC_PROBE_OK,
    PHSTORE_INSTALL_DIAGNOSTIC_PROBE_FAILED,
    PHSTORE_INSTALL_DIAGNOSTIC_RELAY_OK,
    PHSTORE_INSTALL_DIAGNOSTIC_RELAY_FAILED,
    PHSTORE_INSTALL_DIAGNOSTIC_HELPER_OK,
    PHSTORE_INSTALL_DIAGNOSTIC_HELPER_FAILED
} phstore_install_state_t;

typedef struct {
    phstore_install_state_t state;
    char package_id[PHSTORE_PACKAGE_ID_MAX];
    char game_id[PHSTORE_PACKAGE_ID_MAX];
    char source_type[PHSTORE_SOURCE_TYPE_MAX];
    char error[64];
    uint64_t downloaded_bytes;
    uint64_t stream_served_bytes;
    uint64_t header_cache_hits;
    uint64_t header_cache_bytes_served;
    uint64_t header_cache_misses;
    uint64_t total_bytes;
    uint64_t installer_downloaded_bytes;
    uint64_t appinst_downloaded_bytes;
    uint64_t upstream_received_bytes;
    uint64_t head_content_length;
    uint64_t progress_percent_x100;
    uint64_t current_speed_bps;
    uint64_t average_speed_bps;
    double elapsed_seconds;
    double eta_seconds;
    uint64_t range_requests;
    uint64_t head_requests;
    uint64_t current_range_start;
    uint64_t current_range_end;
    uint64_t install_requested_at_ms;
    uint64_t source_resolved_at_ms;
    uint64_t probe_started_at_ms;
    uint64_t probe_completed_at_ms;
    uint64_t helper_start_started_at_ms;
    uint64_t appinst_init_started_at_ms;
    uint64_t install_call_started_at_ms;
    uint64_t waiting_first_range_at_ms;
    uint64_t relay_ready_at_ms;
    uint64_t appinst_called_at_ms;
    uint64_t install_call_completed_at_ms;
    uint64_t first_range_at_ms;
    uint64_t first_upstream_byte_at_ms;
    uint64_t first_stream_byte_at_ms;
    uint64_t phase_started_at_ms;
    uint64_t phase_elapsed_ms;
    uint64_t generation;
    uint64_t finished_at_ms;
    uint32_t errno_code;
    uint64_t relay_head_content_length;
    uint32_t relay_selftest_requests;
    uint32_t relay_selftest_failures;
    int32_t helper_pid;
    int32_t helper_ipc_connect_result;
    int32_t helper_ready_result;
    int32_t appinst_term_result;
    int32_t helper_exit_status;
    int32_t helper_exit_signal;
    uint32_t elfldr_socket_errno;
    uint32_t elfldr_connect_errno;
    uint32_t helper_upload_errno;
    char phase[40];
    char relay_url_path[128];
    char upstream_status[96];
    char last_content_range[96];
    char tls_stage[48];
    int32_t appinst_init_result;
    int32_t install_call_result;
    uint64_t appinst_total_bytes;
    uint32_t appinst_remain_time;
    uint32_t appinst_promote_progress;
    uint32_t retry_attempt;
    uint32_t retry_attempts_total;
    uint32_t retry_delay_seconds;
    uint8_t bulk_transfer_started;
    int32_t appinst_local_copy_percent;
    int32_t appinst_error_code;
    uint8_t appinst_is_copy_only;
    char appinst_status_string[16];
    char appinst_src_type[8];
    char appinst_error_description[512];
    char appinst_error_type[9];
    char appinst_error_name[80];
    char retry_reason[128];
    char install_returned_content_id[48];
    int32_t install_returned_type;
    int32_t install_returned_platform;
    char first_range_method[8];
    char first_range_path[128];
    char first_range_header[96];
    uint8_t relay_listening;
    uint8_t relay_was_ready;
    int32_t relay_head_status_code;
    uint8_t helper_connected;
    uint8_t helper_was_connected;
    uint8_t appinst_initialized;
    uint8_t appinst_initialized_was_true;
    int32_t upstream_native_result;
    int32_t head_status_code;
    int32_t range_probe_status_code;
    char upstream_native_function[48];
    char upstream_scheme[8];
    char upstream_host[256];
    uint16_t upstream_port;
    uint32_t upstream_request_count;
    uint32_t upstream_head_count;
    uint32_t upstream_redirect_count;
    uint32_t upstream_read_failures;
    uint8_t disk_staging;
    uint8_t progress_tracking_limited;
    int32_t native_result;
    int32_t native_error;
} phstore_install_status_t;

/* Looks up only in the active, validated catalog. Returns 1 when found. */
int phstore_catalog_find_install_package(const char *package_id, phstore_package_info_t *package,
                                         char error[64]);
int phstore_catalog_resolve_package_source(phstore_package_info_t *package, char error[64]);

/* Catalog-bound identity lookup for entries without a Title ID. */
int phstore_install_package_title_id(const char *package_id, char title_id[17], char error[64]);

/* 0 queued, nonzero admission error code string. */
const char *phstore_install_start_json(const char *json, size_t length);
int phstore_install_cancel(void);
void phstore_install_get_status(phstore_install_status_t *status);
const char *phstore_install_state_name(phstore_install_state_t state);
void phstore_install_set_phase(const char *phase);
void phstore_install_mark_appinst_called(void);
void phstore_install_set_upstream_stage(const char *stage);
void phstore_install_mark_source_resolved(void);
void phstore_install_mark_probe(int completed);
void phstore_install_mark_relay_ready(void);
void phstore_install_mark_helper_connected(int32_t init_result);
void phstore_install_mark_helper_process(int32_t pid, int stage, int32_t result);
void phstore_install_log_diagnostic_event(const char *phase, const char *message, int error_code);
void phstore_install_mark_install_call_result(int32_t result);
int phstore_install_cancel_requested(void);
char *phstore_install_get_log_json(size_t *length);
char *phstore_install_get_detailed_log_text(size_t *length);
void phstore_install_diagnostics_init(void);
void phstore_install_crash_breadcrumb(const char *event, uint64_t generation);
void phstore_install_detailed_log_reset(const char *package_id);
void phstore_install_detailed_log(const char *event, const char *detail);
int phstore_installer_engine_init(void);
void phstore_installer_engine_shutdown(void);

#endif
