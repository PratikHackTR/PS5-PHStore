#include "phstore_thread.h"
#include "phstore_gdrive.h"
/* PH Store API/status adapter for the vendored ps5-pkg-manager installer. */
#include "phstore_install.h"
#include "phstore_config.h"
#include "phstore_raw_http.h"
#include "phstore_install_request.h"
#include "phstore_notification.h"
#include "installer.h"
#include "stream_server.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_adapter_mutex = PTHREAD_MUTEX_INITIALIZER;
static phstore_package_info_t g_adapter_package;
static uint64_t g_adapter_generation;
static int g_adapter_cancel_requested;
static uint64_t g_last_speed_sample_ms;
static uint64_t g_last_speed_sample_bytes;
static pthread_mutex_t g_summary_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_detailed_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_crash_breadcrumb_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_detailed_log_fd = -1;
static int g_drive_preparing;
static uint64_t g_drive_bytes,g_drive_speed,g_drive_started_ms;
static char g_drive_phase[64];
static int g_admission_active;
static int g_admission_cancelled;
static int g_cancel_worker_active;
static char g_admission_error[64];

/* Metadata lookup shares the raw HTTP resolver, so reserve it against install
 * admission. Never hold the adapter mutex during network or database work. */
static int g_source_query_active;

typedef struct {
    char package_id[PHSTORE_PACKAGE_ID_MAX];
    uint64_t generation;
} install_admission_t;

static uint64_t adapter_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void detailed_log_write_locked(const char *event, const char *detail) {
    if (g_detailed_log_fd < 0) return;
    struct timespec now;
    struct tm utc;
    char timestamp[32] = "time-unavailable";
    long millis = 0;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        if (gmtime_r(&now.tv_sec, &utc)) {
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
            millis = now.tv_nsec / 1000000L;
        }
    }
    char line[768];
    int length = snprintf(line, sizeof(line), "%s.%03ldZ event=%s detail=%s\n",
        timestamp, millis, event ? event : "event", detail ? detail : "-");
    if (length <= 0 || (size_t)length >= sizeof(line)) return;
    size_t written = 0;
    while (written < (size_t)length) {
        ssize_t count = write(g_detailed_log_fd, line + written, (size_t)length - written);
        if (count <= 0) break;
        written += (size_t)count;
    }
}

void phstore_install_detailed_log_reset(const char *package_id) {
    int saved_errno = errno;
    pthread_mutex_lock(&g_detailed_log_mutex);
    if (g_detailed_log_fd >= 0) close(g_detailed_log_fd);
    g_detailed_log_fd = open("/data/phstore2/logs/detailed_log.log",
        O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    detailed_log_write_locked("INSTALL_BEGIN", package_id);
    detailed_log_write_locked("BUILD", PHSTORE_BUILD_ID " backend=native-ps5");
    pthread_mutex_unlock(&g_detailed_log_mutex);
    errno = saved_errno;
}

void phstore_install_detailed_log(const char *event, const char *detail) {
    int saved_errno = errno;
    pthread_mutex_lock(&g_detailed_log_mutex);
    if (g_detailed_log_fd < 0) {
        g_detailed_log_fd = open("/data/phstore2/logs/detailed_log.log",
            O_WRONLY | O_CREAT | O_APPEND, 0600);
    }
    detailed_log_write_locked(event, detail);
    pthread_mutex_unlock(&g_detailed_log_mutex);
    errno = saved_errno;
}

static void install_summary(const char *event, const char *package_id, const char *detail) {
    pthread_mutex_lock(&g_summary_mutex);
    int fd = open("/data/phstore2/logs/latest-install-summary.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        char line[512];
        int length = snprintf(line, sizeof(line), "%s package_id=%s detail=%s\n",
            event ? event : "EVENT", package_id ? package_id : "-", detail ? detail : "-");
        if (length > 0 && (size_t)length < sizeof(line)) {
            (void)write(fd, line, (size_t)length);
            (void)fsync(fd);
        }
        close(fd);
    }
    pthread_mutex_unlock(&g_summary_mutex);
}

int phstore_installer_engine_init(void) {
    char server_url[128];
    snprintf(server_url, sizeof(server_url), "http://%s:%d/", PHSTORE_BIND_IPV4, PHSTORE_HTTP_PORT);
    int result = installer_init(server_url);
    fprintf(stderr, "[PHSTORE/INSTALL] upstream installer_init rc=%d\n", result);
    return result;
}

void phstore_installer_engine_shutdown(void) {
    installer_shutdown();
    fprintf(stderr, "[PHSTORE/INSTALL] upstream installer_shutdown complete\n");
}

static const char *catalog_error_name(const char *error) {
    if (!strcmp(error, "package_not_found")) return "package_not_found";
    if (!strcmp(error, "source_group_unknown")) return "source_group_unknown";
    if (!strcmp(error, "package_source_ambiguous")) return "package_source_ambiguous";
    if (!strcmp(error, "direct_url_invalid")) return "direct_url_invalid";
    if (!strcmp(error, "unsupported_scheme")) return "unsupported_scheme";
    if (!strcmp(error, "package_source_missing")) return "package_source_missing";
    return "catalog_unavailable";
}

static int admission_cancel_requested(void) {
    pthread_mutex_lock(&g_adapter_mutex);
    int cancelled = g_admission_cancelled;
    pthread_mutex_unlock(&g_adapter_mutex);
    return cancelled;
}

static void drive_install_progress(const char *phase,uint64_t bytes,uint64_t total,uint64_t speed) {
    (void)total;pthread_mutex_lock(&g_adapter_mutex);
    g_drive_bytes=bytes;g_drive_speed=speed;snprintf(g_drive_phase,sizeof(g_drive_phase),"%s",phase);
    pthread_mutex_unlock(&g_adapter_mutex);
    if (!strncmp(phase,"error:",6)) phstore_install_detailed_log("NATIVE_DRIVE_ERROR",phase);
}

static void drive_stage_failure(char failure[64]) {
    pthread_mutex_lock(&g_adapter_mutex);
    snprintf(failure,64,"%s",!strncmp(g_drive_phase,"error:",6)?g_drive_phase+6:"native_drive_download_failed");
    pthread_mutex_unlock(&g_adapter_mutex);
}

static void *install_admission_worker(void *opaque) {
    install_admission_t admission = *(install_admission_t *)opaque;
    free(opaque);
    const char *failure = NULL;
    char drive_failure[64] = {0};
    phstore_install_crash_breadcrumb("INSTALL_ADMISSION_WORKER_ENTER", admission.generation);
    phstore_install_detailed_log("ADMISSION_WORKER_ENTER", admission.package_id);
    phstore_package_info_t package;
    char error[64] = {0};
    phstore_install_crash_breadcrumb("CATALOG_LOOKUP_BEGIN", admission.generation);
    phstore_install_detailed_log("CATALOG_LOOKUP_BEGIN", admission.package_id);
    if (admission_cancel_requested()) { failure = "install_cancelled"; goto done; }
    if (phstore_catalog_find_install_package(admission.package_id, &package, error) != 1) {
        phstore_install_crash_breadcrumb("CATALOG_LOOKUP_FAILED", admission.generation);
        failure = catalog_error_name(error);
        phstore_install_detailed_log("CATALOG_LOOKUP_FAILED", error);
        goto done;
    }
    phstore_install_crash_breadcrumb("CATALOG_LOOKUP_OK", admission.generation);
    phstore_install_detailed_log("CATALOG_LOOKUP_OK", package.package_id);
    install_summary("REQUEST", admission.package_id, "catalog-bound request accepted for admission");
    if (!package.installable) { failure = "package_not_installable"; goto done; }
    phstore_install_detailed_log("SOURCE_RESOLVE_BEGIN", package.source_type);
    int uses_https_relay = !strcmp(package.source_type, "direct_http") &&
                          !strncmp(package.download_url, "https://", 8);
    if (phstore_catalog_resolve_package_source(&package, error) != 0) {
        failure = catalog_error_name(error);
        phstore_install_detailed_log("SOURCE_RESOLVE_FAILED", error);
        goto done;
    }
    phstore_install_detailed_log("SOURCE_RESOLVE_DONE", package.source_type);
    if (uses_https_relay)
        phstore_install_detailed_log("SOURCE_HTTPS_RELAY", package.download_url);
    int native_drive=!strcmp(package.source_type,"google_drive_public")&&!strcmp(package.action_type,"install_package");
    if(native_drive) {
        unlink("/data/phstore2/download.cancel");unlink("/data/phstore2/download.cancel.delete");unlink("/data/phstore2/download.pause");
        pthread_mutex_lock(&g_adapter_mutex);g_adapter_package=package;g_drive_preparing=1;g_drive_bytes=0;g_drive_speed=0;g_drive_phase[0]=0;g_drive_started_ms=adapter_monotonic_ms();pthread_mutex_unlock(&g_adapter_mutex);
        phstore_install_detailed_log("NATIVE_DRIVE_STAGE_BEGIN",package.file_id);
        int rc=phstore_gdrive_download(&package,drive_install_progress);
        pthread_mutex_lock(&g_adapter_mutex);g_drive_preparing=0;pthread_mutex_unlock(&g_adapter_mutex);
        drive_stage_failure(drive_failure);
        if(rc){failure=admission_cancel_requested()?"install_cancelled":drive_failure;
            phstore_install_detailed_log("NATIVE_DRIVE_LOG_PATH","/data/phstore2/gdrive/download.log");goto done;}
        phstore_install_detailed_log("NATIVE_DRIVE_STAGE_DONE","exact ranges and full disk read completed; expected SHA optional");
    }
    if (!native_drive && strncmp(package.download_url, "http://", 7) != 0) {
        failure = "unsupported_scheme";
        phstore_install_detailed_log("SOURCE_SCHEME_REJECTED", "expected http");
        goto done;
    }
    if (admission_cancel_requested()) { failure = "install_cancelled"; goto done; }

    pkg_detail_t detail;
    memset(&detail, 0, sizeof(detail));
    int path_len = snprintf(detail.path, sizeof(detail.path), "phstore://%s", package.package_id);
    if (path_len < 0 || (size_t)path_len >= sizeof(detail.path)) { failure = "invalid_package_id"; goto done; }
    snprintf(detail.filename, sizeof(detail.filename), "%s", package.filename);
    snprintf(detail.title_id, sizeof(detail.title_id), "%s", package.title_id);
    if (!detail.title_id[0]) snprintf(detail.title_id, sizeof(detail.title_id), "UNKNOWN");
    snprintf(detail.title_name, sizeof(detail.title_name), "%s", package.game_title);
    snprintf(detail.app_version, sizeof(detail.app_version), "%s", package.version);
    snprintf(detail.pkg_type_str, sizeof(detail.pkg_type_str), "%s", package.package_type);
    detail.pkg_type = !strcmp(package.package_type, "base") ? PKG_TYPE_BASE :
                      !strcmp(package.package_type, "update") ? PKG_TYPE_UPDATE :
                      !strcmp(package.package_type, "dlc") ? PKG_TYPE_DLC : PKG_TYPE_UNKNOWN;
    detail.file_size = package.size_bytes;
    detail.total_pkg_size = package.size_bytes;
    detail.total_parts = 1;
    detail.is_valid = 1;
    if (!detail.filename[0] || !detail.title_id[0] || !detail.title_name[0] || detail.file_size == 0)
        { failure = "package_metadata_incomplete"; phstore_install_detailed_log("PKG_DETAIL_REJECTED", failure); goto done; }

    phstore_install_crash_breadcrumb("PKG_DETAIL_BUILD_OK", admission.generation);
    phstore_install_detailed_log("PKG_DETAIL_BUILD_OK", "metadata fields validated");
    phstore_install_crash_breadcrumb("INSTALLER_START_PHSTORE_ENTER", admission.generation);
    pthread_mutex_lock(&g_adapter_mutex);
    g_adapter_package = package;
    pthread_mutex_unlock(&g_adapter_mutex);
    if (admission_cancel_requested()) { failure = "install_cancelled"; goto done; }
    phstore_install_detailed_log("INSTALLER_START_CALL_BEGIN", detail.path);
    int result = installer_start_phstore(&detail);
    char start_detail[96];
    snprintf(start_detail, sizeof(start_detail), "result=%d (0x%08X)", result, (unsigned)result);
    phstore_install_detailed_log("INSTALLER_START_CALL_RETURN", start_detail);
    if (result == -2) {
        phstore_install_crash_breadcrumb("INSTALLER_START_BUSY", admission.generation);
        failure = "install_in_progress"; goto done;
    }
    if (result != 0) {
        phstore_install_crash_breadcrumb("INSTALLER_START_REJECTED", admission.generation);
        char result_text[48]; snprintf(result_text, sizeof(result_text), "upstream_rc=%d", result);
        install_summary("REJECTED", package.package_id, result_text);
        failure = result == -12 ? "install_start_failed" : "installer_rejected_package";
        goto done;
    }
    phstore_install_detailed_log("INSTALLER_START_ACCEPTED", admission.package_id);
    phstore_install_crash_breadcrumb("INSTALLER_START_ACCEPTED", admission.generation);
    pthread_mutex_lock(&g_adapter_mutex);
    g_adapter_package = package;
    int cancel_after_start = g_adapter_cancel_requested;
    g_last_speed_sample_ms = 0;
    g_last_speed_sample_bytes = 0;
    pthread_mutex_unlock(&g_adapter_mutex);
    if (cancel_after_start) (void)installer_cancel();
    install_log("[PHSTORE] package_id=%s source=%s title_id=%s size=%llu submitted to upstream installer",
        package.package_id, package.source_type, package.title_id,
        (unsigned long long)package.size_bytes);
    install_summary("ACCEPTED", package.package_id, "upstream worker submitted");
done:
    pthread_mutex_lock(&g_adapter_mutex);
    g_admission_active = 0;
    g_admission_cancelled = 0;
    if (failure) {
        snprintf(g_admission_error, sizeof(g_admission_error), "%s", failure);
        g_adapter_cancel_requested = 0;
    } else {
        g_admission_error[0] = '\0';
    }
    pthread_mutex_unlock(&g_adapter_mutex);
    if (failure) {
        install_summary("REJECTED", admission.package_id, failure);
        phstore_install_crash_breadcrumb("INSTALL_ADMISSION_FAILED", admission.generation);
        phstore_install_detailed_log("ADMISSION_FAILED", failure);
    } else {
        phstore_install_detailed_log("ADMISSION_ACCEPTED", admission.package_id);
    }
    return NULL;
}

int phstore_install_package_title_id(const char *package_id, char title_id[17], char error[64]) {
    phstore_package_info_t package;
    title_id[0] = '\0';
    if (phstore_catalog_find_install_package(package_id, &package, error) != 1) return -1;
    if (package.title_id[0] && strcmp(package.title_id, "UNKNOWN") != 0) {
        snprintf(title_id, 17, "%s", package.title_id);
        return 0;
    }
    pthread_mutex_lock(&g_adapter_mutex);
    if (g_source_query_active || g_admission_active || g_cancel_worker_active) {
        pthread_mutex_unlock(&g_adapter_mutex);
        snprintf(error, 64, "installed_lookup_busy"); return -1;
    }
    g_source_query_active = 1;
    pthread_mutex_unlock(&g_adapter_mutex);
    int result = -1;
    installer_status_t status;
    installer_get_status(&status);
    phstore_raw_http_response_t response = {.fd = -1};
    if (status.is_installing) { snprintf(error, 64, "install_in_progress"); goto done; }
    if (phstore_catalog_resolve_package_source(&package, error) != 0) goto done;
    if(package.size_bytes<128){snprintf(error,64,"package_identity_unavailable");goto done;}
    unsigned char header[128];
    if(!strcmp(package.source_type,"google_drive_public")) {
        if(phstore_gdrive_read_prefix(&package,header,sizeof(header))){snprintf(error,64,"gdrive_identity_read_failed");goto done;}
    } else {
        if(strncmp(package.download_url,"http://",7)){snprintf(error,64,"package_identity_unavailable");goto done;}
        if(phstore_raw_http_open_range(&package,0,sizeof(header)-1,&response,error))goto done;
        size_t received=0;while(received<sizeof(header)) {
            int count=phstore_raw_http_read(&response,header+received,sizeof(header)-received);
            if(count<=0){snprintf(error,64,"package_identity_read_failed");goto done;}received+=(size_t)count;
        }
    }
    if (memcmp(header, "\x7f" "CNT", 4) != 0) { snprintf(error, 64, "package_identity_unsupported"); goto done; }
    char content_id[49];
    memcpy(content_id, header + 0x40, 48); content_id[48] = '\0';
    const char *dash = strchr(content_id, '-');
    const char *end = dash ? strchr(dash + 1, '_') : NULL;
    size_t length = end ? (size_t)(end - dash - 1) : 0;
    if (length < 9 || length > 16) { snprintf(error, 64, "package_identity_invalid"); goto done; }
    for (size_t i = 0; i < length; i++) {
        char c = dash[1 + i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            snprintf(error, 64, "package_identity_invalid"); goto done;
        }
    }
    memcpy(title_id, dash + 1, length); title_id[length] = '\0';
    phstore_install_detailed_log("PACKAGE_TITLE_RESOLVED", title_id);
    result = 0;
done:
    phstore_raw_http_close(&response);
    pthread_mutex_lock(&g_adapter_mutex);
    g_source_query_active = 0;
    pthread_mutex_unlock(&g_adapter_mutex);
    return result;
}

const char *phstore_install_start_json(const char *json, size_t length) {
    char package_id[PHSTORE_PACKAGE_ID_MAX];
    if (!json || !phstore_install_request_parse(json, length, package_id)) {
        phstore_install_crash_breadcrumb("INSTALL_JSON_REJECTED", 0);
        return "invalid_install_request";
    }
    phstore_install_crash_breadcrumb("INSTALL_JSON_PARSED", 0);
    install_admission_t *admission = calloc(1, sizeof(*admission));
    if (!admission) return "install_start_failed";
    snprintf(admission->package_id, sizeof(admission->package_id), "%s", package_id);
    pthread_mutex_lock(&g_adapter_mutex);
    if (g_source_query_active) {
        pthread_mutex_unlock(&g_adapter_mutex);
        free(admission);
        return "installed_lookup_busy";
    }
    if (g_admission_active) {
        pthread_mutex_unlock(&g_adapter_mutex);
        free(admission);
        return "install_in_progress";
    }
    g_admission_active = 1;
    g_admission_cancelled = 0;
    g_admission_error[0] = '\0';
    g_adapter_cancel_requested = 0;
    memset(&g_adapter_package, 0, sizeof(g_adapter_package));
    snprintf(g_adapter_package.package_id, sizeof(g_adapter_package.package_id), "%s", package_id);
    admission->generation = ++g_adapter_generation;
    pthread_mutex_unlock(&g_adapter_mutex);

    uint64_t generation = admission->generation;
    phstore_install_detailed_log_reset(package_id);
    phstore_install_detailed_log("ADMISSION_QUEUED", package_id);
    pthread_t worker;
    int create_result = phstore_thread_create(&worker, install_admission_worker, admission);
    if (create_result != 0) {
        char detail[64]; snprintf(detail, sizeof(detail), "pthread_create=%d", create_result);
        phstore_install_detailed_log("ADMISSION_WORKER_CREATE_FAILED", detail);
        pthread_mutex_lock(&g_adapter_mutex);
        g_admission_active = 0;
        snprintf(g_admission_error, sizeof(g_admission_error), "install_start_failed");
        pthread_mutex_unlock(&g_adapter_mutex);
        free(admission);
        return "install_start_failed";
    }
    (void)pthread_detach(worker);
    phstore_install_crash_breadcrumb("INSTALL_ADMISSION_QUEUED", generation);
    return NULL;
}

static void *install_cancel_worker(void *unused) {
    (void)unused;
    (void)installer_cancel();
    pthread_mutex_lock(&g_adapter_mutex);
    g_cancel_worker_active = 0;
    pthread_mutex_unlock(&g_adapter_mutex);
    return NULL;
}

int phstore_install_cancel(void) {
    pthread_mutex_lock(&g_adapter_mutex);
    if (g_admission_active) {
        g_adapter_cancel_requested = 1;
        g_admission_cancelled = 1;
        pthread_mutex_unlock(&g_adapter_mutex);
        return 0;
    }
    if (g_cancel_worker_active) {
        pthread_mutex_unlock(&g_adapter_mutex);
        return 0;
    }
    pthread_mutex_unlock(&g_adapter_mutex);

    installer_status_t current_status;
    installer_get_status(&current_status);
    if (!current_status.is_installing) return 1;

    pthread_mutex_lock(&g_adapter_mutex);
    if (g_admission_active) {
        g_adapter_cancel_requested = 1;
        g_admission_cancelled = 1;
        pthread_mutex_unlock(&g_adapter_mutex);
        return 0;
    }
    if (g_cancel_worker_active) {
        pthread_mutex_unlock(&g_adapter_mutex);
        return 0;
    }
    g_adapter_cancel_requested = 1;
    g_cancel_worker_active = 1;
    pthread_mutex_unlock(&g_adapter_mutex);
    phstore_raw_http_cancel();
    pthread_t worker;
    if (phstore_thread_create(&worker, install_cancel_worker, NULL) != 0) {
        pthread_mutex_lock(&g_adapter_mutex);
        g_cancel_worker_active = 0;
        g_adapter_cancel_requested = 0;
        pthread_mutex_unlock(&g_adapter_mutex);
        return 1;
    }
    (void)pthread_detach(worker);
    return 0;
}

int phstore_install_cancel_requested(void) {
    pthread_mutex_lock(&g_adapter_mutex);
    int cancelled = g_adapter_cancel_requested;
    pthread_mutex_unlock(&g_adapter_mutex);
    return cancelled;
}

void phstore_install_get_status(phstore_install_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    installer_status_t upstream;
    installer_get_status(&upstream);
    pthread_mutex_lock(&g_adapter_mutex);
    phstore_package_info_t package = g_adapter_package;
    out->generation = g_adapter_generation;
    int drive_preparing=g_drive_preparing;uint64_t drive_bytes=g_drive_bytes,drive_speed=g_drive_speed,drive_started_ms=g_drive_started_ms;
    char drive_phase[64];snprintf(drive_phase,sizeof(drive_phase),"%s",g_drive_phase);
    int admission_active = g_admission_active;
    int admission_cancelled = g_admission_cancelled;
    char admission_error[sizeof(g_admission_error)];
    snprintf(admission_error, sizeof(admission_error), "%s", g_admission_error);
    pthread_mutex_unlock(&g_adapter_mutex);
    snprintf(out->package_id, sizeof(out->package_id), "%s", package.package_id);
    snprintf(out->game_id, sizeof(out->game_id), "%s", package.game_id);
    snprintf(out->source_type, sizeof(out->source_type), "%s", package.source_type);
    snprintf(out->phase, sizeof(out->phase), "%s", upstream.status_str);
    snprintf(out->install_returned_content_id, sizeof(out->install_returned_content_id), "%s", upstream.content_id);
    out->total_bytes = upstream.total_bytes ? upstream.total_bytes : package.size_bytes;
    out->downloaded_bytes = upstream.downloaded_bytes;
    out->installer_downloaded_bytes = upstream.downloaded_bytes;
    out->appinst_downloaded_bytes = upstream.downloaded_bytes;
    out->appinst_total_bytes = upstream.total_bytes;
    out->stream_served_bytes = upstream.stream_served_bytes;
    out->progress_percent_x100 = (uint64_t)(upstream.progress_percent * 100.0f);
    out->appinst_remain_time = upstream.appinst_status.remain_time;
    out->appinst_promote_progress = upstream.appinst_status.promote_progress;
    out->appinst_downloaded_bytes = upstream.appinst_status.downloaded_size;
    if (out->appinst_downloaded_bytes > upstream.stream_served_bytes)
        out->appinst_downloaded_bytes = upstream.stream_served_bytes;
    out->appinst_total_bytes = upstream.appinst_status.total_size ? upstream.appinst_status.total_size : upstream.total_bytes;
    out->appinst_local_copy_percent = upstream.appinst_status.local_copy_percent;
    out->appinst_is_copy_only = upstream.appinst_status.is_copy_only;
    snprintf(out->appinst_status_string, sizeof(out->appinst_status_string), "%s", upstream.appinst_status.status);
    snprintf(out->appinst_src_type, sizeof(out->appinst_src_type), "%s", upstream.appinst_status.src_type);
    out->appinst_error_code = upstream.appinst_status.error_info.error_code;
    snprintf(out->appinst_error_description, sizeof(out->appinst_error_description), "%s", upstream.appinst_status.error_info.description);
    snprintf(out->appinst_error_type, sizeof(out->appinst_error_type), "%s", upstream.appinst_status.error_info.type);
    out->current_speed_bps = 0;
    out->average_speed_bps = 0;
    out->elapsed_seconds = upstream.start_time ? difftime(time(NULL), upstream.start_time) : 0;
    if (out->elapsed_seconds < 0) out->elapsed_seconds = 0;
    if (out->elapsed_seconds > 0 && upstream.downloaded_bytes > 0)
        out->average_speed_bps = (uint64_t)((double)upstream.downloaded_bytes / out->elapsed_seconds);
    uint64_t now_ms = adapter_monotonic_ms();
    pthread_mutex_lock(&g_adapter_mutex);
    if (g_last_speed_sample_ms && now_ms > g_last_speed_sample_ms && upstream.downloaded_bytes >= g_last_speed_sample_bytes) {
        out->current_speed_bps = (upstream.downloaded_bytes - g_last_speed_sample_bytes) * 1000u /
                                 (now_ms - g_last_speed_sample_ms);
    }
    if (now_ms > g_last_speed_sample_ms) {
        g_last_speed_sample_ms = now_ms;
        g_last_speed_sample_bytes = upstream.downloaded_bytes;
    }
    pthread_mutex_unlock(&g_adapter_mutex);
    if (upstream.appinst_status.remain_time > 0)
        out->eta_seconds = upstream.appinst_status.remain_time;
    else if (out->current_speed_bps > 0 && out->total_bytes > upstream.downloaded_bytes)
        out->eta_seconds = (double)(out->total_bytes - upstream.downloaded_bytes) / out->current_speed_bps;
    else if (out->average_speed_bps > 0 && out->total_bytes > upstream.downloaded_bytes)
        out->eta_seconds = (double)(out->total_bytes - upstream.downloaded_bytes) / out->average_speed_bps;
    out->range_requests = upstream.stream_served_bytes ? 1 : 0;
    out->finished_at_ms = (upstream.completed || upstream.failed) ? (uint64_t)time(NULL) * 1000u : 0;
    if (admission_active) {
        out->state = admission_cancelled ? PHSTORE_INSTALL_CANCELING : PHSTORE_INSTALL_STARTING;
        snprintf(out->phase, sizeof(out->phase), "%s", admission_cancelled ? "cancelling" : "admission");
    }
    else if (admission_error[0]) {
        out->state = !strcmp(admission_error, "install_cancelled") ? PHSTORE_INSTALL_CANCELLED : PHSTORE_INSTALL_FAILED;
        snprintf(out->phase, sizeof(out->phase), "%s",
            !strcmp(admission_error, "install_cancelled") ? "cancelled" : "admission_failed");
        snprintf(out->error, sizeof(out->error), "%s", admission_error);
    }
    else if (upstream.is_installing) out->state = phstore_install_cancel_requested() ? PHSTORE_INSTALL_CANCELING : PHSTORE_INSTALL_INSTALLING;
    else if (upstream.failed && !strcmp(upstream.status_str, "canceled")) out->state = PHSTORE_INSTALL_CANCELLED;
    else if (upstream.completed) out->state = PHSTORE_INSTALL_COMPLETED;
    else if (upstream.failed) out->state = PHSTORE_INSTALL_FAILED;
    else out->state = PHSTORE_INSTALL_IDLE;
    if (upstream.error_code && !admission_active && !admission_error[0]) {
        out->native_error = upstream.error_code;
        snprintf(out->error, sizeof(out->error), "0x%08X", (unsigned)upstream.error_code);
    }
    if(drive_preparing) {
        out->total_bytes=package.size_bytes;out->downloaded_bytes=drive_bytes;out->current_speed_bps=drive_speed;
        out->installer_downloaded_bytes=0;out->appinst_downloaded_bytes=0;out->stream_served_bytes=0;out->finished_at_ms=0;
        out->elapsed_seconds=now_ms>=drive_started_ms?(now_ms-drive_started_ms)/1000.0:0;
        out->average_speed_bps=out->elapsed_seconds>0?(uint64_t)(drive_bytes/out->elapsed_seconds):0;
        out->eta_seconds=drive_speed&&drive_bytes<package.size_bytes?(double)(package.size_bytes-drive_bytes)/drive_speed:0;
        out->progress_percent_x100=package.size_bytes?(uint64_t)((double)drive_bytes*10000.0/package.size_bytes):0;
        snprintf(out->phase,sizeof(out->phase),"native_drive_%s",drive_phase);
    }
    int last_http_status = 0;
    phstore_raw_http_get_diagnostics(&out->upstream_received_bytes, &out->upstream_request_count,
        &out->upstream_head_count, &out->upstream_redirect_count, &out->upstream_read_failures,
        &last_http_status, &out->head_status_code, &out->head_content_length,
        &out->range_probe_status_code, out->tls_stage, out->last_content_range,
        &out->upstream_native_result);
    if (last_http_status > 0)
        snprintf(out->upstream_status, sizeof(out->upstream_status), "HTTP %d", last_http_status);
    out->range_requests = out->upstream_request_count;
    out->head_requests = out->upstream_head_count;
    out->relay_listening = stream_server_is_running() != 0;
    out->relay_was_ready = out->relay_listening;
}

const char *phstore_install_state_name(phstore_install_state_t state) {
    switch (state) {
        case PHSTORE_INSTALL_STARTING: return "starting";
        case PHSTORE_INSTALL_CANCELING: return "cancelling";
        case PHSTORE_INSTALL_INSTALLING: return "installing";
        case PHSTORE_INSTALL_COMPLETED: return "completed";
        case PHSTORE_INSTALL_FAILED: return "failed";
        case PHSTORE_INSTALL_CANCELLED: return "cancelled";
        default: return "idle";
    }
}

void phstore_install_set_phase(const char *phase) { phstore_install_detailed_log("PHASE", phase); }
void phstore_install_mark_appinst_called(void) {}
void phstore_install_set_upstream_stage(const char *stage) { phstore_install_detailed_log("UPSTREAM_STAGE", stage); }
void phstore_install_mark_source_resolved(void) {}
void phstore_install_mark_probe(int completed) { (void)completed; }
void phstore_install_mark_relay_ready(void) {}
void phstore_install_mark_helper_connected(int32_t result) { (void)result; }
void phstore_install_mark_helper_process(int32_t pid, int stage, int32_t result) { (void)pid; (void)stage; (void)result; }
void phstore_install_log_diagnostic_event(const char *phase, const char *message, int error_code) {
    install_log("[PHSTORE/%s] %s rc=0x%08X", phase ? phase : "INSTALL", message ? message : "", (unsigned)error_code);
}
void phstore_install_mark_install_call_result(int32_t result) { (void)result; }

void phstore_install_diagnostics_init(void) {
    (void)mkdir("/data/phstore2", 0777);
    (void)mkdir("/data/phstore2/logs", 0777);
    install_log_set_file_path("/data/phstore2/logs/install-latest.log");
    int fd = open("/data/phstore2/logs/latest-install-summary.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        static const char index[] =
            "PH Store latest installer diagnostic bundle\n"
            "installer=/data/phstore2/logs/install-latest.log\n"
            "crash_breadcrumbs=/data/phstore2/install-crash.log\n"
            "helper=/data/pkgmgr2/helper-log.txt\n"
            "detailed=/data/phstore2/logs/detailed_log.log\n"
            "images=/data/phstore2/logs/image-cache.log\n"
            "stream=/data/pkgmgr2/stream_debug_<title>_<kind>_<timestamp>_<pid>_<seq>.txt (created when upstream stream debug is enabled)\n"
            "source=/data/phstore2/logs/source-latest.log\n";
        (void)write(fd, index, sizeof(index) - 1); (void)fsync(fd); close(fd);
    }
}

void phstore_install_crash_breadcrumb(const char *event, uint64_t generation) {
    int saved_errno = errno;
    pthread_mutex_lock(&g_crash_breadcrumb_mutex);
    int fd = open("/data/phstore2/install-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) { pthread_mutex_unlock(&g_crash_breadcrumb_mutex); errno = saved_errno; return; }
    char line[256];
    int n = snprintf(line, sizeof(line), "event=%s generation=%llu build=%s pid=%d monotonic_ms=%llu\n",
                     event ? event : "unknown", (unsigned long long)generation,
                     PHSTORE_BUILD_ID, (int)getpid(), (unsigned long long)adapter_monotonic_ms());
    if (n > 0 && (size_t)n < sizeof(line)) {
        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t count = write(fd, line + written, (size_t)n - written);
            if (count <= 0) break;
            written += (size_t)count;
        }
        (void)fsync(fd);
    }
    close(fd);
    pthread_mutex_unlock(&g_crash_breadcrumb_mutex);
    errno = saved_errno;
}

char *phstore_install_get_log_json(size_t *length) {
    size_t text_length = 0;
    char *text = install_log_get_text(&text_length);
    if (!text) return NULL;
    size_t capacity = text_length * 6 + 32;
    char *json = malloc(capacity);
    if (!json) { free(text); return NULL; }
    size_t used = (size_t)snprintf(json, capacity, "{\"log\":\"");
    for (size_t i = 0; i < text_length && used + 7 < capacity; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { json[used++] = '\\'; json[used++] = (char)c; }
        else if (c == '\n') { json[used++] = '\\'; json[used++] = 'n'; }
        else if (c >= 0x20) json[used++] = (char)c;
    }
    json[used++] = '"'; json[used++] = '}'; json[used] = '\0';
    free(text);
    if (length) *length = used;
    return json;
}

char *phstore_install_get_detailed_log_text(size_t *length) {
    const size_t maximum_size = 4u * 1024u * 1024u;
    int fd = open("/data/phstore2/logs/detailed_log.log", O_RDONLY);
    if (fd < 0) return NULL;
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < 0) { close(fd); return NULL; }
    size_t file_size = (size_t)info.st_size;
    size_t read_size = file_size > maximum_size ? maximum_size : file_size;
    int truncated = file_size > maximum_size;
    size_t prefix_size = truncated ? sizeof("[older detail log lines omitted]\n") - 1 : 0;
    char *text = malloc(read_size + prefix_size + 1);
    if (!text) { close(fd); return NULL; }
    size_t used = 0;
    if (truncated) {
        static const char prefix[] = "[older detail log lines omitted]\n";
        memcpy(text, prefix, prefix_size);
        used = prefix_size;
        if (lseek(fd, (off_t)(file_size - read_size), SEEK_SET) < 0) {
            free(text); close(fd); return NULL;
        }
    }
    while (used < read_size + prefix_size) {
        ssize_t count = read(fd, text + used, read_size + prefix_size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
    }
    close(fd);
    text[used] = '\0';
    if (length) *length = used;
    return text;
}
