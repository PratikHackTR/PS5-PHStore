#include "phstore_thread.h"
/*
 * This file includes GPL-3.0-derived PS5 process identity/replacement code
 * adapted from itsPLK/ps5-pkg-manager/src/main.c. See docs/shortcut-porting.md.
 */
#include "phstore_config.h"
#include "phstore_notification.h"
#include "phstore_shortcut.h"
#include "phstore_assets.h"
#include "phstore_catalog.h"
#include "phstore_spectrum.h"
#include "phstore_image_cache.h"
#include "phstore_install.h"
#include "phstore_http_routes.h"
#include "app_info.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_MAX 8192
#define PHSTORE_PROCESS_NAME "phstore2.elf"
#define PROC_TABLE_MAX_BYTES (16u * 1024u * 1024u)
#define PROC_TABLE_MAX_RECORDS 65536u
#define KINFO_PROC_MIN_SIZE 448
#define KINFO_PROC_PID_OFFSET 72
#define KINFO_PROC_TDNAME_OFFSET 447
extern int sceNetInit(void);
extern int sceNetCtlInit(void);
extern int sceUserServiceInitialize(int *priority);
static volatile sig_atomic_t g_resume_requested;
static volatile sig_atomic_t g_shutdown_requested;
static uint64_t g_daemon_started_ms;
static void raw_marker(const char *message, size_t size);

static uint64_t daemon_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void resume_signal_handler(int signal_number) {
    (void)signal_number;
    g_resume_requested = 1;
}

static void shutdown_signal_handler(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
}

static int create_http_listener(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return -1;
    int yes = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PHSTORE_HTTP_PORT);
    raw_marker("[PHSTORE] BEFORE BIND\n", sizeof("[PHSTORE] BEFORE BIND\n") - 1);
    if (inet_pton(AF_INET, PHSTORE_BIND_IPV4, &address.sin_addr) != 1 ||
        bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int saved_errno = errno;
        close(server); errno = saved_errno; return -1;
    }
    raw_marker("[PHSTORE] BEFORE LISTEN\n", sizeof("[PHSTORE] BEFORE LISTEN\n") - 1);
    if (listen(server, 16) != 0) {
        int saved_errno = errno;
        close(server); errno = saved_errno; return -1;
    }
    return server;
}

static int daemon_exit(const char *reason, int code) {
    fprintf(stderr, "[PHSTORE] MAIN LOOP EXITING: %s\n", reason);
    return code;
}

static void raw_marker(const char *message, size_t size) {
    (void)write(STDOUT_FILENO, message, size);
}

/* Adapted from itsPLK/ps5-pkg-manager/src/main.c find_pid(). */
static int find_named_process(const char *name, pid_t *found_pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t capacity = 0;
    *found_pid = -1;
    if (sysctl(mib, 4, NULL, &capacity, NULL, 0) != 0 ||
        capacity < KINFO_PROC_MIN_SIZE || capacity > PROC_TABLE_MAX_BYTES) return -1;
    uint8_t *buffer = malloc(capacity);
    if (!buffer) return -1;
    size_t returned = capacity;
    if (sysctl(mib, 4, buffer, &returned, NULL, 0) != 0 || returned > capacity) {
        free(buffer);
        return -1;
    }
    size_t offset = 0, records = 0;
    while (offset < returned) {
        size_t remaining = returned - offset;
        int record_size = 0;
        if (remaining < sizeof(record_size) || ++records > PROC_TABLE_MAX_RECORDS) {
            free(buffer);
            return -1;
        }
        memcpy(&record_size, buffer + offset, sizeof(record_size));
        if (record_size < KINFO_PROC_MIN_SIZE || (size_t)record_size > remaining) {
            free(buffer);
            return -1;
        }
        pid_t pid;
        memcpy(&pid, buffer + offset + KINFO_PROC_PID_OFFSET, sizeof(pid));
        const char *thread_name = (const char *)(buffer + offset + KINFO_PROC_TDNAME_OFFSET);
        size_t name_bytes = (size_t)record_size - KINFO_PROC_TDNAME_OFFSET;
        if (memchr(thread_name, '\0', name_bytes) && strcmp(name, thread_name) == 0 && pid != getpid())
            *found_pid = pid;
        offset += (size_t)record_size;
    }
    free(buffer);
    return 0;
}

static int replace_previous_instance(void) {
    long name_result = syscall(SYS_thr_set_name, -1, PHSTORE_PROCESS_NAME);
    if (name_result != 0) {
        fprintf(stderr, "[PHSTORE] process naming failed result=%ld errno=%d\n", name_result, errno);
        return -1;
    }
    for (unsigned attempt = 0; attempt < 30; attempt++) {
        pid_t old_pid;
        if (find_named_process(PHSTORE_PROCESS_NAME, &old_pid) != 0) {
            raw_marker("[PHSTORE] process table scan failed; refusing to bind\n",
                       sizeof("[PHSTORE] process table scan failed; refusing to bind\n") - 1);
            return -1;
        }
        if (old_pid <= 0) {
            raw_marker("[PHSTORE] PROCESS CHECK COMPLETE\n", sizeof("[PHSTORE] PROCESS CHECK COMPLETE\n") - 1);
            return 0;
        }
        fprintf(stderr, "[PHSTORE] replacing previous PH Store pid=%d\n", (int)old_pid);
        if (kill(old_pid, SIGKILL) != 0 && errno != ESRCH) {
            fprintf(stderr, "[PHSTORE] kill pid=%d failed errno=%d\n", (int)old_pid, errno);
            return -1;
        }
        sleep(1);
    }
    raw_marker("[PHSTORE] previous-instance replacement timed out\n",
               sizeof("[PHSTORE] previous-instance replacement timed out\n") - 1);
    return -1;
}

static int g_services_ready;
static void send_ready_notification(void) {
    phstore_notify("PHStore - Server aktif\nPort açıldı\nIP & port: %s:%d",PHSTORE_BIND_IPV4,PHSTORE_HTTP_PORT);
}
static int send_all_ready_notification(void) {
    phstore_catalog_status_t status;phstore_catalog_get_status(&status);
    if(!g_services_ready || !status.has_catalog || !phstore_shortcut_is_current())return 0;
    phstore_notify("PHStore2\nKısayol kuruldu\nServisler yüklendi\nPHStore2 hazır");
    return 1;
}

#ifdef PHSTORE_DEBUG_BUILD
#define PHSTORE_BUILD_TYPE "Debug"
#else
#define PHSTORE_BUILD_TYPE "Release"
#endif
static int send_all(int fd, const char *data, size_t size) {
    while (size) {
        ssize_t n = send(fd, data, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

static void respond(int fd, int status, const char *reason, const char *type,
                    const char *body, size_t length, int send_body) {
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; img-src 'self' https: data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'\r\n\r\n",
        status, reason, type, length);
    if (n > 0 && (size_t)n < sizeof(header)) {
        if (send_all(fd, header, (size_t)n) == 0 && send_body)
            send_all(fd, body, length);
    }
}

static const char *catalog_state_name(phstore_catalog_state_t state) {
    switch (state) {
        case PHSTORE_CATALOG_READY: return "ready";
        case PHSTORE_CATALOG_ERROR: return "error";
        case PHSTORE_CATALOG_REFRESHING: return "refreshing";
        case PHSTORE_CATALOG_LOADING:
        default: return "loading";
    }
}

static void respond_catalog_status(int fd) {
    phstore_catalog_status_t status;
    phstore_catalog_get_status(&status);
    char body[512];
    int length = snprintf(body, sizeof(body),
        "{\"ok\":true,\"state\":\"%s\",\"schema_version\":%u,\"catalog_version\":%u,"
        "\"game_count\":%zu,\"generated_at\":\"%s\",\"last_error\":\"%s\"}",
        catalog_state_name(status.state), status.schema_version, status.catalog_version,
        status.game_count, status.generated_at, status.last_error);
    if (length > 0 && (size_t)length < sizeof(body))
        respond(fd, 200, "OK", "application/json", body, (size_t)length, 1);
}

static void json_escape_copy(char *output, size_t capacity, const char *input) {
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)(input ? input : ""); *p && used + 7 < capacity; p++) {
        if (*p == '"' || *p == '\\') { output[used++] = '\\'; output[used++] = (char)*p; }
        else if (*p < 0x20) { int n = snprintf(output + used, capacity - used, "\\u%04x", *p); if (n > 0) used += (size_t)n; }
        else output[used++] = (char)*p;
    }
    if (capacity) output[used < capacity ? used : capacity - 1] = '\0';
}

static void respond_install_status(int fd) {
    phstore_install_status_t status;
    phstore_install_get_status(&status);
    char eta[40];
    if (status.current_speed_bps) snprintf(eta, sizeof(eta), "%.1f", status.eta_seconds);
    else snprintf(eta, sizeof(eta), "null");
    char body[8192], native_description[1200], retry_reason[256];
    json_escape_copy(native_description, sizeof(native_description), status.appinst_error_description);
    json_escape_copy(retry_reason, sizeof(retry_reason), status.retry_reason);
    int length = snprintf(body, sizeof(body),
        "{\"ok\":true,\"state\":\"%s\",\"phase\":\"%s\",\"package_id\":\"%s\",\"game_id\":\"%s\","
        "\"total_bytes\":%llu,\"downloaded_bytes\":%llu,\"stream_served_bytes\":%llu,\"upstream_received_bytes\":%llu,"
        "\"header_cache_hits\":%llu,\"header_cache_bytes_served\":%llu,\"header_cache_misses\":%llu,\"bulk_transfer_started\":%s,"
        "\"logical_progress_bytes\":%llu,\"last_upstream_http_status\":\"%s\","
        "\"head_status\":%d,\"head_content_length\":%llu,\"range_probe_status\":%d,"
        "\"progress_percent\":%.2f,\"current_speed_bps\":%llu,\"average_speed_bps\":%llu,"
        "\"elapsed_seconds\":%.1f,\"eta_seconds\":%s,\"range_requests\":%llu,\"head_requests\":%llu,"
        "\"current_range_start\":%llu,\"current_range_end\":%llu,\"source_type\":\"%s\",\"disk_staging\":false,"
        "\"upstream_status\":\"%s\",\"upstream_request_count\":%u,\"upstream_head_count\":%u,\"upstream_redirect_count\":%u,"
        "\"last_content_range\":\"%s\",\"progress_tracking_limited\":%s,"
        "\"upstream_read_failures\":%u,\"upstream_scheme\":\"%s\",\"upstream_host\":\"%s\",\"upstream_port\":%u,"
        "\"upstream_native_function\":\"%s\",\"upstream_native_result\":%d,\"tls_stage\":\"%s\","
        "\"upstream_stage\":\"%s\","
        "\"install_requested_at_ms\":%llu,\"source_resolved_at_ms\":%llu,\"probe_started_at_ms\":%llu,\"probe_completed_at_ms\":%llu,"
        "\"helper_start_started_at_ms\":%llu,\"appinst_init_started_at_ms\":%llu,\"install_call_started_at_ms\":%llu,\"waiting_first_range_at_ms\":%llu,"
        "\"relay_ready_at_ms\":%llu,\"appinst_called_at_ms\":%llu,\"install_call_completed_at_ms\":%llu,"
        "\"first_range_at_ms\":%llu,\"first_upstream_byte_at_ms\":%llu,\"first_stream_byte_at_ms\":%llu,"
        "\"generation\":%llu,\"phase_started_ms\":%llu,\"phase_elapsed_ms\":%llu,\"finished_at_ms\":%llu,\"errno_code\":%u,\"relay_listening\":%s,\"relay_was_ready\":%s,"
        "\"relay_url_path\":\"%s\",\"relay_head_status\":%d,\"relay_head_content_length\":%llu,"
        "\"relay_selftest_requests\":%u,\"relay_selftest_failures\":%u,\"helper_connected\":%s,"
        "\"helper_was_connected\":%s,\"helper_pid\":%d,\"helper_ipc_connect_result\":%d,\"helper_ready_result\":%d,"
        "\"appinst_initialized\":%s,\"appinst_initialized_was_true\":%s,\"appinst_term_result\":%d,"
        "\"elfldr_socket_errno\":%u,\"elfldr_connect_errno\":%u,\"helper_upload_errno\":%u,"
        "\"helper_exit_status\":%d,\"helper_exit_signal\":%d,"
        "\"appinst_init_result\":%d,\"install_call_result\":%d,\"daemon_uptime_seconds\":%llu,"
        "\"appinst_downloaded_bytes\":%llu,\"appinst_total_bytes\":%llu,\"appinst_status_string\":\"%s\","
        "\"appinst_src_type\":\"%s\",\"appinst_remain_time\":%u,\"appinst_promote_progress\":%u,"
        "\"appinst_local_copy_percent\":%d,\"appinst_is_copy_only\":%s,\"appinst_error_code\":%d,"
        "\"appinst_error_description\":\"%s\",\"appinst_error_type\":\"%s\","
        "\"appinst_error_name\":\"%s\",\"retry_reason\":\"%s\",\"retry_attempt\":%u,\"retry_attempts_total\":%u,\"retry_delay_seconds\":%u,"
        "\"install_returned_content_id\":\"%s\",\"install_returned_type\":%d,\"install_returned_platform\":%d,"
        "\"first_range_method\":\"%s\",\"first_range_path\":\"%s\",\"first_range_header\":\"%s\","
        "\"error\":\"%s\",\"native_error_code\":\"0x%08X\",\"native_result\":%d}",
        phstore_install_state_name(status.state), status.phase, status.package_id, status.game_id,
        (unsigned long long)status.total_bytes, (unsigned long long)status.downloaded_bytes,
        (unsigned long long)status.stream_served_bytes, (unsigned long long)status.upstream_received_bytes,
        (unsigned long long)status.header_cache_hits, (unsigned long long)status.header_cache_bytes_served,
        (unsigned long long)status.header_cache_misses, status.bulk_transfer_started ? "true" : "false",
        (unsigned long long)status.downloaded_bytes, status.upstream_status,
        status.head_status_code, (unsigned long long)status.head_content_length, status.range_probe_status_code,
        (double)status.progress_percent_x100 / 100.0,
        (unsigned long long)status.current_speed_bps, (unsigned long long)status.average_speed_bps,
        status.elapsed_seconds, eta, (unsigned long long)status.range_requests, (unsigned long long)status.head_requests,
        (unsigned long long)status.current_range_start, (unsigned long long)status.current_range_end,
        status.source_type, status.upstream_status, status.upstream_request_count, status.upstream_head_count, status.upstream_redirect_count,
        status.last_content_range, status.progress_tracking_limited ? "true" : "false",
        status.upstream_read_failures, status.upstream_scheme, status.upstream_host, status.upstream_port,
        status.upstream_native_function, status.upstream_native_result, status.tls_stage,
        status.upstream_native_function,
        (unsigned long long)status.install_requested_at_ms, (unsigned long long)status.source_resolved_at_ms,
        (unsigned long long)status.probe_started_at_ms, (unsigned long long)status.probe_completed_at_ms,
        (unsigned long long)status.helper_start_started_at_ms, (unsigned long long)status.appinst_init_started_at_ms,
        (unsigned long long)status.install_call_started_at_ms,
        (unsigned long long)status.waiting_first_range_at_ms,
        (unsigned long long)status.relay_ready_at_ms, (unsigned long long)status.appinst_called_at_ms,
        (unsigned long long)status.install_call_completed_at_ms, (unsigned long long)status.first_range_at_ms,
        (unsigned long long)status.first_upstream_byte_at_ms, (unsigned long long)status.first_stream_byte_at_ms,
        (unsigned long long)status.generation, (unsigned long long)status.phase_started_at_ms,
        (unsigned long long)status.phase_elapsed_ms, (unsigned long long)status.finished_at_ms,
        status.errno_code, status.relay_listening ? "true" : "false",
        status.relay_was_ready ? "true" : "false", status.relay_url_path,
        status.relay_head_status_code, (unsigned long long)status.relay_head_content_length,
        status.relay_selftest_requests, status.relay_selftest_failures,
        status.helper_connected ? "true" : "false",
        status.helper_was_connected ? "true" : "false", status.helper_pid,
        status.helper_ipc_connect_result, status.helper_ready_result,
        status.appinst_initialized ? "true" : "false",
        status.appinst_initialized_was_true ? "true" : "false", status.appinst_term_result,
        status.elfldr_socket_errno, status.elfldr_connect_errno, status.helper_upload_errno,
        status.helper_exit_status, status.helper_exit_signal,
        status.appinst_init_result, status.install_call_result,
        (unsigned long long)((daemon_monotonic_ms() - g_daemon_started_ms) / 1000u),
        (unsigned long long)status.appinst_downloaded_bytes, (unsigned long long)status.appinst_total_bytes,
        status.appinst_status_string, status.appinst_src_type, status.appinst_remain_time,
        status.appinst_promote_progress, status.appinst_local_copy_percent,
        status.appinst_is_copy_only ? "true" : "false", status.appinst_error_code,
        native_description, status.appinst_error_type, status.appinst_error_name, retry_reason,
        status.retry_attempt, status.retry_attempts_total, status.retry_delay_seconds,
        status.install_returned_content_id, status.install_returned_type, status.install_returned_platform,
        status.first_range_method, status.first_range_path, status.first_range_header,
        status.error, (unsigned)status.native_error, status.native_result);
    if (length > 0 && (size_t)length < sizeof(body))
        respond(fd, 200, "OK", "application/json", body, (size_t)length, 1);
}

static int valid_installed_title_id(const char *text, size_t length) {
    if (!text || length < 9 || length > 16) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static void respond_installed_state(int fd, const char *title_id, size_t title_id_length) {
    if (!valid_installed_title_id(title_id, title_id_length)) {
        static const char bad[] = "{\"ok\":false,\"error\":\"invalid_title_id\"}";
        respond(fd, 400, "Bad Request", "application/json", bad, sizeof(bad) - 1, 1);
        return;
    }
    char id[17], version[32] = {0};
    memcpy(id, title_id, title_id_length); id[title_id_length] = '\0';
    int installed = app_info_check_installed(id, version, sizeof(version));
    char body[256];
    int length = snprintf(body, sizeof(body),
        "{\"ok\":true,\"title_id\":\"%s\",\"is_installed\":%s,\"installed_version\":\"%s\","
        "\"can_install\":%s,\"disabled_reason\":\"%s\"}",
        id, installed ? "true" : "false", version,
        installed ? "false" : "true", installed ? "already_installed" : "");
    if (length > 0 && (size_t)length < sizeof(body))
        respond(fd, 200, "OK", "application/json", body, (size_t)length, 1);
}

static void respond_installed_package(int fd, const char *package_id) {
    size_t length = strlen(package_id);
    if (!length || length >= PHSTORE_PACKAGE_ID_MAX) {
        static const char body[] = "{\"ok\":false,\"error\":\"invalid_package_id\"}";
        respond(fd, 400, "Bad Request", "application/json", body, sizeof(body)-1, 1); return;
    }
    for (size_t i = 0; i < length; i++) {
        char c = package_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            static const char body[] = "{\"ok\":false,\"error\":\"invalid_package_id\"}";
            respond(fd, 400, "Bad Request", "application/json", body, sizeof(body)-1, 1); return;
        }
    }
    char title_id[17], error[64] = {0};
    if (phstore_install_package_title_id(package_id, title_id, error) != 0) {
        char body[160];
        int count = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", error[0] ? error : "installed_state_unknown");
        respond(fd, 503, "Service Unavailable", "application/json", body, (size_t)count, 1); return;
    }
    respond_installed_state(fd, title_id, strlen(title_id));
}

static void handle_client(int fd) {
    char request[REQUEST_MAX + 1];
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    struct timeval send_timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    size_t used = 0;
    size_t header_end = 0;
    size_t content_length = 0;
    while (used < REQUEST_MAX) {
        ssize_t n = recv(fd, request + used, REQUEST_MAX - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        used += (size_t)n;
        request[used] = '\0';
        char *end = strstr(request, "\r\n\r\n");
        if (!end) end = strstr(request, "\n\n");
        if (end) {
            header_end = (size_t)(end - request) + (end[0] == '\r' ? 4 : 2);
            char *header = strstr(request, "Content-Length:");
            if (!header) header = strstr(request, "content-length:");
            if (header && header < end) {
                char *value = strchr(header, ':') + 1;
                while (*value == ' ' || *value == '\t') value++;
                char *tail = NULL;
                unsigned long parsed = strtoul(value, &tail, 10);
                if (tail != value && parsed <= REQUEST_MAX - header_end) content_length = (size_t)parsed;
                else if (parsed != 0) content_length = REQUEST_MAX;
            }
            if (used >= header_end + content_length) break;
        }
    }
    if (used == REQUEST_MAX) {
        static const char body[] = "Request Too Large\n";
        respond(fd, 413, "Content Too Large", "text/plain", body, sizeof(body)-1, 1);
        return;
    }
    char method[8] = {0}, path[512] = {0};
    if (sscanf(request, "%7s %511s", method, path) != 2) {
        static const char body[] = "Bad Request\n";
        respond(fd, 400, "Bad Request", "text/plain", body, sizeof(body)-1, 1);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        respond(fd, 200, "OK", "text/html; charset=utf-8", (const char *)phstore_frontend_html, phstore_frontend_html_size, 1);
    } else if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && strcmp(path, "/api/health") == 0) {
        char body[256];
        int length = snprintf(body, sizeof(body),
            "{\"ok\":true,\"version\":\"%s\",\"build\":\"%s\",\"loopback_only\":true,\"uptime_seconds\":%llu,\"pid\":%d}",
            PHSTORE_VERSION, PHSTORE_BUILD_ID, (unsigned long long)((daemon_monotonic_ms() - g_daemon_started_ms) / 1000u), (int)getpid());
        respond(fd, 200, "OK", "application/json", body, (size_t)length,
                strcmp(method, "HEAD") != 0);
    } else if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && strncmp(path, "/resimler/", 10) == 0) {
        size_t length = 0; const char *mime = NULL;
        unsigned char *data = phstore_image_cache_read(path + 10, &length, &mime);
        if (data) { respond(fd, 200, "OK", mime, (const char *)data, length, strcmp(method, "HEAD") != 0); free(data); }
        else { static const char missing[] = "Image not cached"; respond(fd, 404, "Not Found", "text/plain", missing, sizeof(missing)-1, strcmp(method, "HEAD") != 0); }
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/images/log") == 0) {
        size_t length = 0; char *body = phstore_image_cache_log_text(&length);
        if (body) { respond(fd, 200, "OK", "text/plain; charset=utf-8", body, length, 1); free(body); }
        else { static const char missing[] = "No image cache log yet. Start Cache al first."; respond(fd, 404, "Not Found", "text/plain", missing, sizeof(missing)-1, 1); }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/store/images/cache") == 0) {
        int result = phstore_image_cache_start(0);
        const char *body = result == 0 ? "{\"ok\":true}" : result == 1 ? "{\"ok\":false,\"error\":\"cache_in_progress\"}" : "{\"ok\":false,\"error\":\"cache_unavailable\"}";
        respond(fd, result == 0 ? 202 : result == 1 ? 409 : 503, result == 0 ? "Accepted" : result == 1 ? "Conflict" : "Service Unavailable", "application/json", body, strlen(body), 1);
    } else if (strcmp(method, "GET") == 0 && (strcmp(path, "/api/store/images/status") == 0 || strcmp(path, "/api/store/images/index") == 0)) {
        char *body = !strcmp(path, "/api/store/images/status") ? phstore_image_cache_status_json() : phstore_image_cache_index_json();
        if (body) { respond(fd, 200, "OK", "application/json", body, strlen(body), 1); free(body); }
        else { static const char error[] = "{\"ok\":false,\"error\":\"cache_unavailable\"}"; respond(fd, 503, "Service Unavailable", "application/json", error, sizeof(error)-1, 1); }
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/catalog/status") == 0) {
        respond_catalog_status(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/install/status") == 0) {
        respond_install_status(fd);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/store/installed?package_id=", sizeof("/api/store/installed?package_id=")-1) == 0) {
        respond_installed_package(fd, path + sizeof("/api/store/installed?package_id=")-1);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/store/installed?title_id=", 30) == 0) {
        const char *title_id = path + 30;
        size_t title_id_length = strcspn(title_id, "&");
        respond_installed_state(fd, title_id, title_id_length);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/install/log") == 0) {
        size_t log_length = 0;
        char *log_body = phstore_install_get_log_json(&log_length);
        if (log_body) { respond(fd, 200, "OK", "application/json", log_body, log_length, 1); free(log_body); }
        else { static const char body[] = "{\"ok\":false,\"error\":\"out_of_memory\"}"; respond(fd, 500, "Internal Server Error", "application/json", body, sizeof(body)-1, 1); }
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/install/detailed-log") == 0) {
        size_t log_length = 0;
        char *log_body = phstore_install_get_detailed_log_text(&log_length);
        if (log_body) { respond(fd, 200, "OK", "text/plain; charset=utf-8", log_body, log_length, 1); free(log_body); }
        else { static const char body[] = "Detailed install log is not available yet.\n"; respond(fd, 404, "Not Found", "text/plain; charset=utf-8", body, sizeof(body)-1, 1); }
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/spectrum/status") == 0) {
        char body[1024]; size_t n = phstore_spectrum_status_json(body, sizeof(body));
        respond(fd, 200, "OK", "application/json", body, n, 1);
    } else if (!strcmp(method,"POST") && (!strcmp(path,"/api/store/spectrum/resume") || !strcmp(path,"/api/store/spectrum/cancel"))) {
        int ok=!strcmp(path,"/api/store/spectrum/resume")?phstore_spectrum_resume():phstore_spectrum_cancel_delete();
        const char *body=ok==0?"{\"ok\":true}":"{\"ok\":false,\"error\":\"download_control_failed\"}";
        respond(fd,ok==0?202:409,"Download","application/json",body,strlen(body),1);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/store/spectrum/pause") == 0) {
        int ok = phstore_spectrum_pause();
        const char *body = ok == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no_active_download\"}";
        respond(fd, ok == 0 ? 202 : 409, "Download", "application/json", body, strlen(body), 1);
    } else if (strcmp(method, "POST") == 0 && (!strcmp(path, "/api/store/spectrum/download") || !strcmp(path,"/api/store/download"))) {
        const char *error = !header_end || used < header_end || content_length != used - header_end
            ? "invalid_install_request" : phstore_spectrum_start_json(request + header_end, content_length);
        char body[180]; int n = snprintf(body, sizeof(body), error ? "{\"ok\":false,\"error\":\"%s\"}" : "{\"ok\":true}", error ? error : "");
        respond(fd, error ? 409 : 202, "Download", "application/json", body, (size_t)n, 1);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/store/install") == 0) {
        uint64_t install_post_started_ms = daemon_monotonic_ms();
        phstore_install_crash_breadcrumb("INSTALL_CLICK_RECEIVED", 0);
        if (!header_end || used < header_end || content_length != used - header_end) {
            static const char bad[] = "{\"ok\":false,\"error\":\"invalid_install_request\"}";
            respond(fd, 400, "Bad Request", "application/json", bad, sizeof(bad)-1, 1);
        } else {
            const char *error = phstore_spectrum_install_start_json(request + header_end, content_length);
            if (!error) {
                static const char accepted[] = "{\"ok\":true,\"state\":\"starting\"}";
                respond(fd, 202, "Accepted", "application/json", accepted, sizeof(accepted)-1, 1);
                phstore_install_crash_breadcrumb("HTTP_RESPONSE_SENT", 0);
                fprintf(stderr, "[PHSTORE/API] install_post_response_ms=%llu result=accepted\n",
                        (unsigned long long)(daemon_monotonic_ms() - install_post_started_ms));
            } else {
                phstore_install_crash_breadcrumb("INSTALL_ADMISSION_REJECTED", 0);
                char body[160];
                int length = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", error);
                int code = !strcmp(error, "package_not_found") ? 404 :
                    (!strcmp(error, "install_in_progress") || !strcmp(error, "install_cleanup_in_progress") ? 409 : 400);
                if (length > 0 && (size_t)length < sizeof(body))
                    respond(fd, code, code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Bad Request",
                            "application/json", body, (size_t)length, 1);
                fprintf(stderr, "[PHSTORE/API] install_post_response_ms=%llu result=%s\n",
                        (unsigned long long)(daemon_monotonic_ms() - install_post_started_ms), error);
            }
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/store/install/cancel") == 0) {
        int result = phstore_install_cancel();
        const char *body = result == 0 ? "{\"ok\":true,\"state\":\"cancelling\"}" :
                                        "{\"ok\":false,\"error\":\"no_active_install\"}";
        respond(fd, result == 0 ? 202 : 409, result == 0 ? "Accepted" : "Conflict",
                "application/json", body, strlen(body), 1);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/store/game?id=", 19) == 0) {
        size_t game_length = 0;
        char *game = phstore_catalog_game_copy(path + 19, &game_length);
        if (game) {
            respond(fd, 200, "OK", "application/json", game, game_length, 1);
            free(game);
        }
        else {
            static const char missing[] = "{\"ok\":false,\"error\":\"game_not_found\"}";
            respond(fd, 404, "Not Found", "application/json", missing, sizeof(missing)-1, 1);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/store/catalog/refresh") == 0) {
        int refresh = phstore_catalog_refresh();
        if (refresh == 0) {
            phstore_catalog_status_t status;
            phstore_catalog_get_status(&status);
            char body[96];
            int length = snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"%s\"}", catalog_state_name(status.state));
            respond(fd, 202, "Accepted", "application/json", body, (size_t)length, 1);
        } else if (refresh == 1) {
            static const char body[] = "{\"ok\":false,\"error\":\"refresh_in_progress\"}";
            respond(fd, 409, "Conflict", "application/json", body, sizeof(body)-1, 1);
        } else {
            static const char body[] = "{\"ok\":false,\"error\":\"refresh_start_failed\"}";
            respond(fd, 500, "Internal Server Error", "application/json", body, sizeof(body)-1, 1);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/store/shortcut/install") == 0) {
#if PHSTORE_SHORTCUT_ENABLED
        uint32_t error_code = 0;
        int result = phstore_shortcut_force_install(&error_code);
        if (result == 0) {
            (void)send_all_ready_notification();
            static const char body[] = "{\"ok\":true}";
            respond(fd, 200, "OK", "application/json", body, sizeof(body) - 1, 1);
        } else {
            char body[160];
            int length = snprintf(body, sizeof(body),
                "{\"ok\":false,\"error\":\"shortcut_install_failed\",\"code\":\"0x%08X\"}",
                (unsigned)error_code);
            if (length > 0 && (size_t)length < sizeof(body))
                respond(fd, 500, "Internal Server Error", "application/json", body, (size_t)length, 1);
        }
#else
        static const char body[] = "{\"ok\":false,\"error\":\"diagnostic_stage_disables_shortcut_helper\"}";
        respond(fd, 503, "Service Unavailable", "application/json", body, sizeof(body) - 1, 1);
#endif
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/store/shortcut/status") == 0) {
        static const char installed[] = "{\"ok\":true,\"installed\":true}";
        static const char missing[] = "{\"ok\":true,\"installed\":false}";
        int current = phstore_shortcut_is_current();
        const char *body = current ? installed : missing;
        respond(fd, 200, "OK", "application/json", body, strlen(body), 1);
    } else if (strcmp(method, "GET") == 0 &&
               (strcmp(path, "/api/store/info") == 0 || strcmp(path, "/api/store/status") == 0)) {
        phstore_catalog_status_t status;
        phstore_catalog_get_status(&status);
        char body[768];
        int length = snprintf(body, sizeof(body),
            "{\"version\":\"%s\",\"build_type\":\"%s\",\"local_server\":\"ready\","
            "\"catalog_status\":\"%s\",\"catalog_state\":\"%s\",\"catalog_version\":%u,"
            "\"game_count\":%zu,\"generated_at\":\"%s\",\"last_error\":\"%s\"}",
            PHSTORE_VERSION, PHSTORE_BUILD_TYPE, catalog_state_name(status.state), catalog_state_name(status.state),
            status.catalog_version, status.game_count, status.generated_at, status.last_error);
        if (length > 0 && (size_t)length < sizeof(body))
            respond(fd, 200, "OK", "application/json", body, (size_t)length, 1);
    } else if (phstore_request_path_matches(method, path, "GET", "/api/store/catalog")) {
        phstore_catalog_status_t status;
        phstore_catalog_get_status(&status);
        size_t catalog_length = 0;
        char *catalog = phstore_catalog_copy(&catalog_length);
        phstore_catalog_route_result_t result = phstore_catalog_route_result(
            catalog != NULL, status.state == PHSTORE_CATALOG_ERROR);
        if (result == PHSTORE_CATALOG_ROUTE_READY) {
            respond(fd, 200, "OK", "application/json; charset=utf-8", catalog, catalog_length, 1);
            free(catalog);
        } else if (result == PHSTORE_CATALOG_ROUTE_ERROR) {
            static const char body[] = "{\"ok\":false,\"state\":\"error\",\"error\":\"catalog_unavailable\"}";
            respond(fd, 503, "Service Unavailable", "application/json", body, sizeof(body)-1, 1);
        } else {
            static const char body[] = "{\"ok\":false,\"state\":\"loading\"}";
            respond(fd, 503, "Service Unavailable", "application/json", body, sizeof(body)-1, 1);
        }
    } else if (strcmp(method, "GET") == 0) {
        for (size_t i = 0; i < PHSTORE_EMBEDDED_ASSET_COUNT; i++) {
            if (strcmp(path, phstore_embedded_assets[i].path) == 0) {
                respond(fd, 200, "OK", phstore_embedded_assets[i].mime,
                        (const char *)phstore_embedded_assets[i].data,
                        phstore_embedded_assets[i].size, 1);
                return;
            }
        }
        static const char body[] = "Not Found\n";
        respond(fd, 404, "Not Found", "text/plain", body, sizeof(body)-1, 1);
    } else {
        static const char body[] = "Not Found\n";
        respond(fd, 404, "Not Found", "text/plain", body, sizeof(body)-1, 1);
    }
}

static void *client_worker(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle_client(fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

#if PHSTORE_SHORTCUT_ENABLED
static void *shortcut_install_worker(void *unused) {
    (void)unused;
    uint32_t error_code = 0;
    int result = phstore_shortcut_install_if_needed(&error_code);
    if (result != 0) fprintf(stderr, "[PHSTORE/SHORTCUT] automatic install failed code=0x%08X\n", (unsigned)error_code);
    else {
        for(int i=0;i<120;i++) {
            if(send_all_ready_notification())return NULL;
            if(!g_services_ready)break;
            usleep(500000);
        }
        phstore_notify("PHStore2\nKısayol kuruldu; servis veya katalog hazırlığı tamamlanamadı.");
    }
    return NULL;
}

static void start_shortcut_setup(void) {
    pthread_t worker;
    int error = phstore_thread_create(&worker, shortcut_install_worker, NULL);
    if (error != 0) {
        fprintf(stderr, "[PHSTORE/SHORTCUT] setup worker creation failed errno=%d\n", error);
        phstore_notify("PH Store\nKısayol kurulamadı: 0x%08X", (unsigned)error);
        return;
    }
    (void)pthread_detach(worker);
}
#endif

int main(void) {
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGHUP, SIG_IGN);
    (void)signal(SIGTERM, SIG_IGN);
    (void)signal(SIGINT, shutdown_signal_handler);
    (void)signal(SIGCONT, resume_signal_handler);
    g_daemon_started_ms = daemon_monotonic_ms();
    static const char entered[] = "[PHSTORE] ENTERED MAIN\n";
    raw_marker(entered, sizeof(entered) - 1);
    phstore_install_diagnostics_init();
    phstore_catalog_init();
    if (replace_previous_instance() != 0) return daemon_exit("fatal_init_failure", 5);
    raw_marker("[PHSTORE] BEFORE NET INIT\n", sizeof("[PHSTORE] BEFORE NET INIT\n") - 1);
    int net_result = sceNetInit();
    raw_marker("[PHSTORE] AFTER NET INIT\n", sizeof("[PHSTORE] AFTER NET INIT\n") - 1);
    if (net_result != 0) {
        fprintf(stderr, "sceNetInit failed: 0x%08x\n", (unsigned)net_result);
        phstore_notify("PH Store ağ başlatma hatası: 0x%08x", (unsigned)net_result);
        return daemon_exit("fatal_init_failure", 1);
    }
    fprintf(stderr, "[PHSTORE/PROCESS] pid=%d ppid=%d uid=%d euid=%d gid=%d egid=%d\n",
            (int)getpid(), (int)getppid(), (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid());
    int netctl_result = sceNetCtlInit();
    fprintf(stderr, "[PHSTORE] sceNetCtlInit rc=0x%08X (%d)\n", (unsigned)netctl_result, netctl_result);
    int user_priority = 256;
    int user_service_result = sceUserServiceInitialize(&user_priority);
    fprintf(stderr, "[PHSTORE] sceUserServiceInitialize rc=0x%08X (%d)\n",
            (unsigned)user_service_result, user_service_result);
    int server = create_http_listener();
    if (server < 0) {
        int saved_errno = errno;
        perror("socket");
        phstore_notify("PH Store socket hatası: errno %d", saved_errno);
        return daemon_exit("server_start_failure", 1);
    }
    raw_marker("[PHSTORE] HTTP LISTEN READY\n", sizeof("[PHSTORE] HTTP LISTEN READY\n") - 1);
    send_ready_notification();
    int engine_result=phstore_installer_engine_init();
    if(engine_result!=0) {
        fprintf(stderr,"[PHSTORE/INSTALL] upstream engine init failed; HTTP service remains active\n");
        phstore_notify("PHStore2\nSunucu aktif; kurulum servisi başlatılamadı: 0x%08X",(unsigned)engine_result);
    }
    g_services_ready=engine_result==0;
    raw_marker("[PHSTORE] ENTERING ACCEPT LOOP\n", sizeof("[PHSTORE] ENTERING ACCEPT LOOP\n") - 1);
    fprintf(stderr, "PH Store %s listening at http://%s:%d/\n",
            PHSTORE_VERSION, PHSTORE_BIND_IPV4, PHSTORE_HTTP_PORT);
    if (phstore_catalog_start() != 0) {
        g_services_ready=0;
        raw_marker("[PHSTORE/CATALOG] worker start failed; server remains available\n",
                   sizeof("[PHSTORE/CATALOG] worker start failed; server remains available\n") - 1);
        phstore_notify("PHStore2\nSunucu aktif; katalog servisi başlatılamadı.");
    }
#if PHSTORE_SHORTCUT_ENABLED
    start_shortcut_setup();
#endif
    for (;;) {
        if (g_shutdown_requested) break;
        if (g_resume_requested) {
            g_resume_requested = 0;
            close(server);
            server = -1;
            fprintf(stderr, "[PHSTORE] resume detected; recreating loopback listener\n");
        }
        if (server < 0) {
            server = create_http_listener();
            if (server < 0) { fprintf(stderr, "[PHSTORE] listener restart failed errno=%d\n", errno); sleep(2); continue; }
            fprintf(stderr, "[PHSTORE] loopback listener restarted at %s:%d\n", PHSTORE_BIND_IPV4, PHSTORE_HTTP_PORT);
        }
        struct pollfd listener = {.fd = server, .events = POLLIN};
        int ready = poll(&listener, 1, 5000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) {
            int accepting = 0; socklen_t option_length = sizeof(accepting);
            if (getsockopt(server, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &option_length) != 0 || !accepting) {
                fprintf(stderr, "[PHSTORE] watchdog detected dead listener errno=%d\n", errno);
                close(server); server = -1;
            }
            continue;
        }
        if (ready < 0 || !(listener.revents & POLLIN)) {
            fprintf(stderr, "[PHSTORE] watchdog poll failure revents=0x%x errno=%d\n", listener.revents, errno);
            close(server); server = -1; continue;
        }
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; fprintf(stderr, "[PHSTORE] accept failed errno=%d; rebuilding listener\n", errno); close(server); server = -1; continue; }
        pthread_t thread;
        if (phstore_thread_create(&thread, client_worker, (void *)(intptr_t)client) == 0)
            pthread_detach(thread);
        else {
            static const char busy[] = "{\"ok\":false,\"error\":\"server_busy\"}";
            fprintf(stderr, "[PHSTORE] client worker creation failed errno=%d; rejecting connection\n", errno);
            respond(client, 503, "Service Unavailable", "application/json", busy, sizeof(busy) - 1, 1);
            shutdown(client, SHUT_RDWR);
            close(client);
        }
    }
    close(server);
    phstore_installer_engine_shutdown();
    return daemon_exit("explicit_shutdown", 0);
}
