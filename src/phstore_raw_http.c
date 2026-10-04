/* Bounded HTTP/1.1 Range reader for cleartext upstream package sources. */
#include "phstore_raw_http.h"
#include "phstore_raw_image_http.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include "phstore_range.h"
#include "phstore_config.h"
#include "phstore_url.h"
#include "phstore_install.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RAW_HEADER_CAPACITY 16384
#define RAW_CONNECT_TIMEOUT_MS 10000
#define RAW_DNS_TIMEOUT_MS 10000
#define RAW_IO_TIMEOUT_MS 15000
#define RAW_REDIRECT_MAX 5
#define HEADER_CACHE_SIZE (1024u * 1024u)

typedef struct {
    uint64_t bytes;
    uint32_t requests, heads, redirects, read_failures;
    int status;
    int head_status, range_probe_status;
    uint64_t head_size;
    int32_t native_result;
    char stage[48];
    char last_content_range[96];
    uint64_t header_cache_hits, header_cache_bytes_served, header_cache_misses;
} raw_diag_t;

typedef struct address_cache_entry {
    struct address_cache_entry *next;
    char host[256], port[8];
    struct addrinfo *addresses;
    int valid;
} address_cache_entry_t;

static pthread_mutex_t g_address_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static address_cache_entry_t *g_address_cache;
static int g_address_cache_enabled;
static pthread_mutex_t g_header_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_header_cache_condition = PTHREAD_COND_INITIALIZER;
static unsigned char g_header_cache[HEADER_CACHE_SIZE];
static char g_header_cache_url[PHSTORE_DOWNLOAD_URL_MAX];
static uint64_t g_header_cache_total;
static uint64_t g_header_cache_fill_connect_ms, g_header_cache_fill_header_ms, g_header_cache_fill_body_ms;
static uint64_t g_header_cache_fill_bytes;
static uint64_t g_header_cache_fill_first_byte_at_ms;
static int g_header_cache_state; /* 0 empty, 1 filling, 2 ready */

static pthread_mutex_t g_raw_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static raw_diag_t g_raw_diag;
static pthread_mutex_t g_resolver_slot_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_resolver_active;
static pthread_mutex_t g_active_socket_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_active_sockets[16] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};

static void track_socket(int fd) {
    pthread_mutex_lock(&g_active_socket_mutex);
    for (size_t i = 0; i < sizeof(g_active_sockets) / sizeof(g_active_sockets[0]); i++)
        if (g_active_sockets[i] < 0) { g_active_sockets[i] = fd; break; }
    pthread_mutex_unlock(&g_active_socket_mutex);
}

static void close_tracked_socket(int fd) {
    pthread_mutex_lock(&g_active_socket_mutex);
    for (size_t i = 0; i < sizeof(g_active_sockets) / sizeof(g_active_sockets[0]); i++)
        if (g_active_sockets[i] == fd) { g_active_sockets[i] = -1; break; }
    close(fd);
    pthread_mutex_unlock(&g_active_socket_mutex);
}

void phstore_raw_http_cancel(void) {
    pthread_mutex_lock(&g_active_socket_mutex);
    for (size_t i = 0; i < sizeof(g_active_sockets) / sizeof(g_active_sockets[0]); i++)
        if (g_active_sockets[i] >= 0) (void)shutdown(g_active_sockets[i], SHUT_RDWR);
    pthread_mutex_unlock(&g_active_socket_mutex);
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int references, done, result;
    struct addrinfo *addresses;
    char host[256], port[8];
} resolver_job_t;

static uint64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void set_stage(const char *stage) {
    pthread_mutex_lock(&g_raw_diag_mutex);
    snprintf(g_raw_diag.stage, sizeof(g_raw_diag.stage), "%s", stage);
    pthread_mutex_unlock(&g_raw_diag_mutex);
    phstore_install_set_upstream_stage(stage);
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 2
    fprintf(stderr, "[PHSTORE/HTTP] t=%llu stage=%s\n", (unsigned long long)now_ms(), stage);
#endif
}

static void invalidate_endpoint(const char *host, const char *port) {
    if (!host || !port) return;
    pthread_mutex_lock(&g_address_cache_mutex);
    for (address_cache_entry_t *entry = g_address_cache; entry; entry = entry->next)
        if (!strcmp(entry->host, host) && !strcmp(entry->port, port)) entry->valid = 0;
    pthread_mutex_unlock(&g_address_cache_mutex);
}

static void raw_error(const char *stage, int error) {
    pthread_mutex_lock(&g_raw_diag_mutex);
    snprintf(g_raw_diag.stage, sizeof(g_raw_diag.stage), "%s", stage);
    g_raw_diag.native_result = -error;
    pthread_mutex_unlock(&g_raw_diag_mutex);
    phstore_install_set_upstream_stage(stage);
    char detail[160];
    snprintf(detail, sizeof(detail), "stage=%s errno=%d message=%s", stage, error, strerror(error));
    phstore_install_detailed_log("HTTP_ERROR", detail);
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 2
    fprintf(stderr, "[PHSTORE/HTTP] t=%llu %s errno=%d\n", (unsigned long long)now_ms(), stage, error);
#endif
}

static void resolver_release(resolver_job_t *job) {
    int release;
    pthread_mutex_lock(&job->mutex);
    release = --job->references == 0;
    pthread_mutex_unlock(&job->mutex);
    if (release) {
        if (job->addresses) freeaddrinfo(job->addresses);
        pthread_cond_destroy(&job->condition);
        pthread_mutex_destroy(&job->mutex);
        free(job);
    }
}

static void *resolver_worker(void *argument) {
    resolver_job_t *job = argument;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *addresses = NULL;
    int result = getaddrinfo(job->host, job->port, &hints, &addresses);
    pthread_mutex_lock(&job->mutex);
    job->addresses = addresses;
    job->result = result;
    job->done = 1;
    pthread_cond_signal(&job->condition);
    pthread_mutex_unlock(&job->mutex);
    pthread_mutex_lock(&g_resolver_slot_mutex);
    g_resolver_active = 0;
    pthread_mutex_unlock(&g_resolver_slot_mutex);
    resolver_release(job);
    return NULL;
}

static int resolve_bounded(const char *host, const char *port, struct addrinfo **addresses) {
    pthread_mutex_lock(&g_resolver_slot_mutex);
    if (g_resolver_active) { pthread_mutex_unlock(&g_resolver_slot_mutex); errno = EBUSY; return -1; }
    g_resolver_active = 1;
    pthread_mutex_unlock(&g_resolver_slot_mutex);
    resolver_job_t *job = calloc(1, sizeof(*job));
    if (!job) { pthread_mutex_lock(&g_resolver_slot_mutex); g_resolver_active = 0; pthread_mutex_unlock(&g_resolver_slot_mutex); errno = ENOMEM; return -1; }
    if (pthread_mutex_init(&job->mutex, NULL) != 0) {
        free(job); pthread_mutex_lock(&g_resolver_slot_mutex); g_resolver_active = 0;
        pthread_mutex_unlock(&g_resolver_slot_mutex); errno = EAGAIN; return -1;
    }
    if (pthread_cond_init(&job->condition, NULL) != 0) {
        pthread_mutex_destroy(&job->mutex); free(job);
        pthread_mutex_lock(&g_resolver_slot_mutex); g_resolver_active = 0;
        pthread_mutex_unlock(&g_resolver_slot_mutex); errno = EAGAIN; return -1;
    }
    job->references = 2;
    snprintf(job->host, sizeof(job->host), "%s", host);
    snprintf(job->port, sizeof(job->port), "%s", port);
    pthread_t thread;
    if (pthread_create(&thread, NULL, resolver_worker, job) != 0) {
        pthread_mutex_lock(&g_resolver_slot_mutex); g_resolver_active = 0; pthread_mutex_unlock(&g_resolver_slot_mutex);
        resolver_release(job); resolver_release(job); errno = EAGAIN; return -1;
    }
    pthread_detach(thread);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += RAW_DNS_TIMEOUT_MS / 1000;
    pthread_mutex_lock(&job->mutex);
    int wait_result = 0;
    while (!job->done && wait_result == 0) wait_result = pthread_cond_timedwait(&job->condition, &job->mutex, &deadline);
    int completed = job->done;
    int lookup_result = completed ? job->result : EAI_AGAIN;
    if (completed && lookup_result == 0) { *addresses = job->addresses; job->addresses = NULL; }
    pthread_mutex_unlock(&job->mutex);
    resolver_release(job);
    if (!completed || lookup_result != 0) { errno = completed ? EHOSTUNREACH : ETIMEDOUT; return -1; }
    return 0;
}

void phstore_raw_http_reset_diagnostics(void) {
    pthread_mutex_lock(&g_raw_diag_mutex);
    memset(&g_raw_diag, 0, sizeof(g_raw_diag));
    pthread_mutex_unlock(&g_raw_diag_mutex);
}

void phstore_raw_http_source_begin(const phstore_package_info_t *package) {
    pthread_mutex_lock(&g_address_cache_mutex);
    address_cache_entry_t *entry = g_address_cache;
    g_address_cache = NULL;
    while (entry) {
        address_cache_entry_t *next = entry->next;
        if (entry->addresses) freeaddrinfo(entry->addresses);
        free(entry);
        entry = next;
    }
    g_address_cache_enabled = 1;
    pthread_mutex_unlock(&g_address_cache_mutex);
    pthread_mutex_lock(&g_header_cache_mutex);
    g_header_cache_state = 0;
    g_header_cache_url[0] = '\0';
    g_header_cache_total = 0;
    g_header_cache_fill_connect_ms = g_header_cache_fill_header_ms = g_header_cache_fill_body_ms = 0;
    g_header_cache_fill_bytes = g_header_cache_fill_first_byte_at_ms = 0;
    if (package) {
        snprintf(g_header_cache_url, sizeof(g_header_cache_url), "%s", package->download_url);
        g_header_cache_total = package->size_bytes;
    }
    pthread_mutex_unlock(&g_header_cache_mutex);
    pthread_mutex_lock(&g_raw_diag_mutex);
    g_raw_diag.header_cache_hits = g_raw_diag.header_cache_bytes_served = g_raw_diag.header_cache_misses = 0;
    pthread_mutex_unlock(&g_raw_diag_mutex);
}

void phstore_raw_http_source_end(void) {
    pthread_mutex_lock(&g_address_cache_mutex);
    address_cache_entry_t *entry = g_address_cache;
    g_address_cache = NULL;
    while (entry) {
        address_cache_entry_t *next = entry->next;
        if (entry->addresses) freeaddrinfo(entry->addresses);
        free(entry);
        entry = next;
    }
    g_address_cache_enabled = 0;
    pthread_mutex_unlock(&g_address_cache_mutex);
    pthread_mutex_lock(&g_header_cache_mutex);
    g_header_cache_state = 0;
    g_header_cache_url[0] = '\0';
    g_header_cache_total = 0;
    g_header_cache_fill_bytes = g_header_cache_fill_first_byte_at_ms = 0;
    pthread_cond_broadcast(&g_header_cache_condition);
    pthread_mutex_unlock(&g_header_cache_mutex);
}

void phstore_raw_http_get_cache_diagnostics(uint64_t *hits, uint64_t *bytes_served,
                                            uint64_t *misses) {
    pthread_mutex_lock(&g_raw_diag_mutex);
    if (hits) *hits = g_raw_diag.header_cache_hits;
    if (bytes_served) *bytes_served = g_raw_diag.header_cache_bytes_served;
    if (misses) *misses = g_raw_diag.header_cache_misses;
    pthread_mutex_unlock(&g_raw_diag_mutex);
}

void phstore_raw_http_get_diagnostics(uint64_t *bytes, uint32_t *requests, uint32_t *heads,
                                      uint32_t *redirects, uint32_t *read_failures,
                                      int *status, int *head_status, uint64_t *head_size,
                                      int *range_probe_status, char stage[48], char content_range[96],
                                      int32_t *native_result) {
    pthread_mutex_lock(&g_raw_diag_mutex);
    if (bytes) *bytes = g_raw_diag.bytes;
    if (requests) *requests = g_raw_diag.requests;
    if (heads) *heads = g_raw_diag.heads;
    if (redirects) *redirects = g_raw_diag.redirects;
    if (read_failures) *read_failures = g_raw_diag.read_failures;
    if (status) *status = g_raw_diag.status;
    if (head_status) *head_status = g_raw_diag.head_status;
    if (head_size) *head_size = g_raw_diag.head_size;
    if (range_probe_status) *range_probe_status = g_raw_diag.range_probe_status;
    if (stage) snprintf(stage, 48, "%s", g_raw_diag.stage);
    if (content_range) snprintf(content_range, 96, "%s", g_raw_diag.last_content_range);
    if (native_result) *native_result = g_raw_diag.native_result;
    pthread_mutex_unlock(&g_raw_diag_mutex);
}

static int wait_fd(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = now_ms();
        if (!now || now >= deadline) { errno = ETIMEDOUT; return -1; }
        uint64_t remaining = deadline - now;
        struct pollfd descriptor = {.fd = fd, .events = events};
        int result = poll(&descriptor, 1, remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { if (!result) errno = ETIMEDOUT; return -1; }
        if (descriptor.revents & events) return 0;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            int socket_error = 0; socklen_t length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error)
                errno = socket_error;
            else if (descriptor.revents & POLLNVAL) errno = EBADF;
            else errno = ECONNRESET;
            return -1;
        }
    }
}

static int parse_http_url(const char *url, char host[256], char port[8], char path[2048]) {
    if (!phstore_url_validate(url) || strncasecmp(url, "http://", 7)) return 0;
    const char *authority = url + 7;
    const char *end = authority + strcspn(authority, "/?");
    const char *host_start = authority, *host_end = end, *port_start = NULL;
    if (*host_start == '[') {
        const char *close = memchr(host_start, ']', (size_t)(end - host_start));
        if (!close) return 0;
        host_start++; host_end = close;
        if (close + 1 < end) port_start = close + 2;
    } else {
        for (const char *p = authority; p < end; p++) if (*p == ':') {
            host_end = p; port_start = p + 1; break;
        }
    }
    size_t host_length = (size_t)(host_end - host_start);
    if (!host_length || host_length >= 256) return 0;
    memcpy(host, host_start, host_length); host[host_length] = '\0';
    snprintf(port, 8, "%s", port_start ? port_start : "80");
    if (end == url + strlen(url)) snprintf(path, 2048, "/");
    else if (*end == '?') snprintf(path, 2048, "/%s", end);
    else snprintf(path, 2048, "%s", end);
    return 1;
}

static int connect_http(const char *host, const char *port, int ranged_request, uint64_t *connect_ms) {
    uint64_t started = now_ms();
    struct addrinfo *addresses = NULL;
    int cached = 0, cache_enabled = 0, addresses_owned = 0;
    pthread_mutex_lock(&g_address_cache_mutex);
    cache_enabled = g_address_cache_enabled;
    if (cache_enabled) {
        for (address_cache_entry_t *entry = g_address_cache; entry; entry = entry->next) {
            if (entry->valid && !strcmp(entry->host, host) && !strcmp(entry->port, port)) {
                addresses = entry->addresses; cached = 1; break;
            }
        }
    }
    pthread_mutex_unlock(&g_address_cache_mutex);
    if (!cache_enabled) {
        set_stage("http_dns");
        if (resolve_bounded(host, port, &addresses) != 0) { raw_error("DNS_failed", errno ? errno : EHOSTUNREACH); return -1; }
        addresses_owned = 1;
    } else if (cached) set_stage("http_endpoint_cached");
    else {
        set_stage("http_dns");
        if (resolve_bounded(host, port, &addresses) != 0) { raw_error("DNS_failed", errno ? errno : EHOSTUNREACH); return -1; }
        address_cache_entry_t *created = calloc(1, sizeof(*created));
        if (!created) { freeaddrinfo(addresses); errno = ENOMEM; return -1; }
        snprintf(created->host, sizeof(created->host), "%s", host);
        snprintf(created->port, sizeof(created->port), "%s", port);
        created->addresses = addresses; created->valid = 1;
        pthread_mutex_lock(&g_address_cache_mutex);
        address_cache_entry_t *existing = NULL;
        for (address_cache_entry_t *entry = g_address_cache; entry; entry = entry->next)
            if (entry->valid && !strcmp(entry->host, host) && !strcmp(entry->port, port)) { existing = entry; break; }
        if (existing) { pthread_mutex_unlock(&g_address_cache_mutex); freeaddrinfo(created->addresses); free(created); addresses = existing->addresses; }
        else { created->next = g_address_cache; g_address_cache = created; pthread_mutex_unlock(&g_address_cache_mutex); }
    }
    set_stage(ranged_request ? "http_range_connect" : "http_connect");
    int fd = -1;
    uint64_t connect_deadline = now_ms() + RAW_CONNECT_TIMEOUT_MS;
    for (struct addrinfo *entry = addresses; entry; entry = entry->ai_next) {
        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fd); fd = -1; continue;
        }
        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, connect_deadline) != 0) {
            close(fd); fd = -1; continue;
        }
        int socket_error = 0; socklen_t length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 || socket_error) {
            if (socket_error) errno = socket_error;
            close(fd); fd = -1; continue;
        }
        break;
    }
    if (fd < 0 && cache_enabled) {
        pthread_mutex_lock(&g_address_cache_mutex);
        for (address_cache_entry_t *entry = g_address_cache; entry; entry = entry->next)
            if (!strcmp(entry->host, host) && !strcmp(entry->port, port) && entry->addresses == addresses) entry->valid = 0;
        pthread_mutex_unlock(&g_address_cache_mutex);
    }
    if (addresses_owned) freeaddrinfo(addresses);
    if (fd < 0) raw_error("connect_failed", errno ? errno : ECONNREFUSED);
    else set_stage("http_connected");
    if (connect_ms) *connect_ms = now_ms() - started;
    return fd;
}

static int send_bounded(int fd, const char *data, size_t length) {
    uint64_t deadline = now_ms() + RAW_IO_TIMEOUT_MS;
    while (length) {
        if (wait_fd(fd, POLLOUT, deadline) != 0) return -1;
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { if (!count) errno = EPIPE; return -1; }
        data += count; length -= (size_t)count;
    }
    return 0;
}

static int header_value(const char *headers, const char *name, char *out, size_t capacity) {
    size_t name_len = strlen(name);
    const char *line = strstr(headers, "\r\n");
    if (!line) return 0;
    line += 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        if (!end) return 0;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon && (size_t)(colon - line) == name_len && !strncasecmp(line, name, name_len)) {
            const char *value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            size_t size = (size_t)(end - value);
            while (size && (value[size - 1] == ' ' || value[size - 1] == '\t')) size--;
            if (size >= capacity) return 0;
            memcpy(out, value, size); out[size] = '\0'; return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int read_response_headers(int fd, char buffer[RAW_HEADER_CAPACITY + 1], size_t *length,
                                 size_t *body_offset) {
    *length = 0; *body_offset = 0;
    uint64_t deadline = now_ms() + RAW_IO_TIMEOUT_MS;
    while (*length < RAW_HEADER_CAPACITY) {
        if (wait_fd(fd, POLLIN, deadline) != 0) return -1;
        ssize_t count = recv(fd, buffer + *length, RAW_HEADER_CAPACITY - *length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { if (!count) errno = ECONNRESET; return -1; }
        *length += (size_t)count;
        buffer[*length] = '\0';
        char *end = strstr(buffer, "\r\n\r\n");
        if (end) { *body_offset = (size_t)(end + 4 - buffer); return 0; }
    }
    errno = EMSGSIZE;
    return -1;
}

static int parse_u64_text(const char *text, uint64_t *value) {
    if (!text || !*text) return 0;
    uint64_t result = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9' || result > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return 0;
        result = result * 10 + (unsigned)(*p - '0');
    }
    *value = result; return 1;
}

typedef struct {
    int fd, status;
    uint64_t content_length;
    char headers[RAW_HEADER_CAPACITY + 1];
    char content_range[96];
    unsigned char first_body[RAW_HEADER_CAPACITY];
    size_t first_body_length;
    uint64_t connect_ms, header_wait_ms;
    char endpoint_host[256], endpoint_port[8];
} raw_response_t;

static int raw_request(const char *initial_url, const char *method, int has_range,
                       uint64_t start, uint64_t end, raw_response_t *response,
                       char final_url[PHSTORE_DOWNLOAD_URL_MAX], char error[64]) {
    snprintf(final_url, PHSTORE_DOWNLOAD_URL_MAX, "%s", initial_url);
    for (unsigned redirect = 0; redirect <= RAW_REDIRECT_MAX; redirect++) {
        char host[256], port[8], path[2048];
        if (!parse_http_url(final_url, host, port, path)) { snprintf(error, 64, "direct_url_invalid"); return -1; }
        pthread_mutex_lock(&g_raw_diag_mutex);
        g_raw_diag.requests++;
        if (!strcmp(method, "HEAD")) g_raw_diag.heads++;
        pthread_mutex_unlock(&g_raw_diag_mutex);
        uint64_t connect_ms = 0;
        int fd = connect_http(host, port, has_range, &connect_ms);
        if (fd < 0) { snprintf(error, 64, "upstream_connect_failed"); return -1; }
        snprintf(response->endpoint_host, sizeof(response->endpoint_host), "%s", host);
        snprintf(response->endpoint_port, sizeof(response->endpoint_port), "%s", port);
        track_socket(fd);
        char request[4096];
        char host_header[320];
        if (strchr(host, ':')) snprintf(host_header, sizeof(host_header), "[%s]%s%s", host,
            strcmp(port, "80") ? ":" : "", strcmp(port, "80") ? port : "");
        else snprintf(host_header, sizeof(host_header), "%s%s%s", host,
            strcmp(port, "80") ? ":" : "", strcmp(port, "80") ? port : "");
        int length = has_range
            ? snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PHStore/1.0\r\nAccept-Encoding: identity\r\nRange: bytes=%llu-%llu\r\nConnection: close\r\n\r\n", method, path, host_header, (unsigned long long)start, (unsigned long long)end)
            : snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PHStore/1.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", method, path, host_header);
        set_stage(!strcmp(method, "HEAD") ? "http_head_send" : has_range ? "http_range_send" : "http_get_send");
        if (length <= 0 || (size_t)length >= sizeof(request) || send_bounded(fd, request, (size_t)length) != 0) {
            int saved = errno ? errno : EINVAL; raw_error("request_send_failed", saved); close_tracked_socket(fd);
            invalidate_endpoint(host, port);
            snprintf(error, 64, "upstream_connect_failed"); return -1;
        }
        set_stage(!strcmp(method, "HEAD") ? "http_head_headers" : has_range ? "http_range_headers" : "http_get_headers");
        size_t header_length = 0, body_offset = 0;
        uint64_t header_started = now_ms();
        if (read_response_headers(fd, response->headers, &header_length, &body_offset) != 0) {
            int saved = errno ? errno : EIO; raw_error("response_headers_failed", saved); close_tracked_socket(fd);
            invalidate_endpoint(host, port);
            snprintf(error, 64, "upstream_http_error"); return -1;
        }
        response->connect_ms += connect_ms;
        response->header_wait_ms += now_ms() - header_started;
        int status = 0;
        if (sscanf(response->headers, "HTTP/%*u.%*u %d", &status) != 1) {
            close_tracked_socket(fd); snprintf(error, 64, "upstream_http_error"); return -1;
        }
        pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.status = status; pthread_mutex_unlock(&g_raw_diag_mutex);
        response->status = status;
        response->content_length = 0;
        char value[PHSTORE_DOWNLOAD_URL_MAX];
        if (header_value(response->headers, "Content-Length", value, sizeof(value)))
            (void)parse_u64_text(value, &response->content_length);
        response->content_range[0] = '\0';
        if (header_value(response->headers, "Content-Range", value, sizeof(value))) {
            snprintf(response->content_range, sizeof(response->content_range), "%s", value);
            pthread_mutex_lock(&g_raw_diag_mutex);
            snprintf(g_raw_diag.last_content_range, sizeof(g_raw_diag.last_content_range), "%s", value);
            pthread_mutex_unlock(&g_raw_diag_mutex);
        }
        response->fd = fd;
        response->first_body_length = header_length - body_offset;
        if (response->first_body_length) memcpy(response->first_body, response->headers + body_offset, response->first_body_length);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (redirect == RAW_REDIRECT_MAX || !header_value(response->headers, "Location", value, sizeof(value))) {
                close_tracked_socket(fd); response->fd = -1; snprintf(error, 64, "redirect_limit"); return -1;
            }
            char resolved[PHSTORE_DOWNLOAD_URL_MAX];
            if (!phstore_url_resolve_redirect(final_url, value, resolved, sizeof(resolved)) || strncasecmp(resolved, "http://", 7)) {
                close_tracked_socket(fd); response->fd = -1; snprintf(error, 64, "direct_url_invalid"); return -1;
            }
            pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.redirects++; pthread_mutex_unlock(&g_raw_diag_mutex);
            close_tracked_socket(fd); response->fd = -1;
            snprintf(final_url, PHSTORE_DOWNLOAD_URL_MAX, "%s", resolved);
            continue;
        }
        return 0;
    }
    snprintf(error, 64, "redirect_limit"); return -1;
}

static int validate_range(const raw_response_t *response, uint64_t start, uint64_t end,
                          uint64_t total, uint64_t *content_length) {
    uint64_t wanted = end - start + 1;
    if (!phstore_content_range_validate(response->status, response->content_range,
        response->content_length, start, end, total)) return 0;
    *content_length = wanted; return 1;
}

int phstore_raw_http_probe(const phstore_package_info_t *package, char error[64]) {
    if (!package || !package->download_url[0] || !package->size_bytes) { snprintf(error, 64, "package_source_missing"); return -1; }
    raw_response_t *response = calloc(1, sizeof(*response));
    if (!response) { snprintf(error, 64, "out_of_memory"); return -1; }
    response->fd = -1;
    char final_url[PHSTORE_DOWNLOAD_URL_MAX];
    set_stage("http_head_send");
    if (raw_request(package->download_url, "HEAD", 0, 0, 0, response, final_url, error) != 0) { free(response); return -1; }
    int head_status = response->status;
    uint64_t head_size = response->content_length;
    close_tracked_socket(response->fd);
    free(response);
    set_stage("http_head_complete");
    pthread_mutex_lock(&g_raw_diag_mutex);
    g_raw_diag.head_status = head_status;
    g_raw_diag.head_size = head_size;
    pthread_mutex_unlock(&g_raw_diag_mutex);
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 3
    phstore_install_set_phase("head_complete");
#endif
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 2
    fprintf(stderr, "[PHSTORE/HTTP] HEAD_status=%d content_length=%llu\n", head_status, (unsigned long long)head_size);
#endif
    if (head_status != 200 || head_size != package->size_bytes) {
        errno = EPROTO;
        raw_error("head_validation_failed", errno);
        snprintf(error, 64, "upstream_http_error"); return -1;
    }
    raw_response_t *range = calloc(1, sizeof(*range));
    if (!range) { snprintf(error, 64, "out_of_memory"); return -1; }
    range->fd = -1;
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 3
    phstore_install_set_phase("range_probe_begin");
#endif
    set_stage("http_range_probe");
    if (raw_request(package->download_url, "GET", 1, 0, 1023, range, final_url, error) != 0) { free(range); return -1; }
    pthread_mutex_lock(&g_raw_diag_mutex); int status = g_raw_diag.status; pthread_mutex_unlock(&g_raw_diag_mutex);
    pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.range_probe_status = status; pthread_mutex_unlock(&g_raw_diag_mutex);
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 2
    fprintf(stderr, "[PHSTORE/HTTP] range_probe_status=%d content_range=%s\n", status, range->content_range);
#endif
    uint64_t length = 0;
    int valid = validate_range(range, 0, 1023, package->size_bytes, &length) && length == 1024;
    if (!valid) {
        if (range->fd >= 0) close_tracked_socket(range->fd);
        free(range); errno = EPROTO; raw_error("range_validation_failed", errno);
        snprintf(error, 64, "upstream_range_invalid"); return -1;
    }
    phstore_raw_http_response_t *body = calloc(1, sizeof(*body));
    if (!body) { close_tracked_socket(range->fd); free(range); snprintf(error, 64, "out_of_memory"); return -1; }
    body->fd = range->fd; body->remaining = 1024;
    body->connect_ms = range->connect_ms; body->header_wait_ms = range->header_wait_ms;
    snprintf(body->endpoint_host, sizeof(body->endpoint_host), "%s", range->endpoint_host);
    snprintf(body->endpoint_port, sizeof(body->endpoint_port), "%s", range->endpoint_port);
    body->buffered_length = range->first_body_length;
    if (body->buffered_length) memcpy(body->buffered, range->first_body, body->buffered_length);
    free(range);
    set_stage("http_range_body");
    unsigned char buffer[1024];
    size_t received = 0;
    while (received < sizeof(buffer)) {
        int count = phstore_raw_http_read(body, buffer + received, sizeof(buffer) - received);
        if (count <= 0) break;
        received += (size_t)count;
    }
    if (received != sizeof(buffer)) {
        phstore_raw_http_close(body); free(body);
        if (!errno) errno = EIO;
        raw_error("range_body_incomplete", errno);
        snprintf(error, 64, "upstream_range_invalid"); return -1;
    }
    phstore_raw_http_close(body); free(body);
#if PHSTORE_INSTALL_DIAGNOSTIC_STAGE != 3
    phstore_install_set_phase("range_probe_complete");
#endif
    set_stage("range_probe_completed");
    return 0;
}

static int open_range_uncached(const phstore_package_info_t *package, uint64_t start,
                               uint64_t end, phstore_raw_http_response_t *response,
                               char error[64]) {
    if (!package || !response || start > end || end >= package->size_bytes) { snprintf(error, 64, "upstream_range_invalid"); return -1; }
    raw_response_t *raw = calloc(1, sizeof(*raw));
    if (!raw) { snprintf(error, 64, "out_of_memory"); return -1; }
    raw->fd = -1;
    char final_url[PHSTORE_DOWNLOAD_URL_MAX];
    if (raw_request(package->download_url, "GET", 1, start, end, raw, final_url, error) != 0) { free(raw); return -1; }
    uint64_t length = 0;
    if (!validate_range(raw, start, end, package->size_bytes, &length)) {
        invalidate_endpoint(raw->endpoint_host, raw->endpoint_port);
        close_tracked_socket(raw->fd); free(raw); errno = EPROTO;
        snprintf(error, 64, "upstream_range_invalid"); return -1;
    }
    memset(response, 0, sizeof(*response));
    response->fd = raw->fd; response->remaining = length;
    response->connect_ms = raw->connect_ms; response->header_wait_ms = raw->header_wait_ms;
    snprintf(response->endpoint_host, sizeof(response->endpoint_host), "%s", raw->endpoint_host);
    snprintf(response->endpoint_port, sizeof(response->endpoint_port), "%s", raw->endpoint_port);
    response->buffered_length = raw->first_body_length;
    if (response->buffered_length) memcpy(response->buffered, raw->first_body, response->buffered_length);
    free(raw);
    return 0;
}

static void copy_header_cache_range(uint64_t start, uint64_t end,
                                    phstore_raw_http_response_t *response, int hit,
                                    uint64_t bytes_received) {
    size_t length = (size_t)(end - start + 1);
    memset(response, 0, sizeof(*response));
    response->fd = -1;
    response->remaining = length;
    response->cache_backed = 1;
    response->cache_offset = start;
    response->header_cache_hit = (uint8_t)hit;
    response->bytes_received = bytes_received;
    if (hit) {
        pthread_mutex_lock(&g_raw_diag_mutex);
        g_raw_diag.header_cache_hits++;
        pthread_mutex_unlock(&g_raw_diag_mutex);
    }
    pthread_mutex_lock(&g_raw_diag_mutex);
    g_raw_diag.header_cache_bytes_served += length;
    pthread_mutex_unlock(&g_raw_diag_mutex);
}

int phstore_raw_http_open_range(const phstore_package_info_t *package, uint64_t start,
                                uint64_t end, phstore_raw_http_response_t *response,
                                char error[64]) {
    if (!package || !response || start > end || end >= package->size_bytes) {
        snprintf(error, 64, "upstream_range_invalid"); return -1;
    }
    if (end < HEADER_CACHE_SIZE && start < HEADER_CACHE_SIZE) {
        int fill = 0, ready = 0;
        pthread_mutex_lock(&g_header_cache_mutex);
        if (strcmp(g_header_cache_url, package->download_url) == 0 &&
            g_header_cache_total == package->size_bytes && package->size_bytes >= HEADER_CACHE_SIZE) {
            while (g_header_cache_state == 1 && !phstore_install_cancel_requested())
                pthread_cond_wait(&g_header_cache_condition, &g_header_cache_mutex);
            if (g_header_cache_state == 0 && !phstore_install_cancel_requested()) {
                g_header_cache_state = 1; fill = 1;
            } else if (g_header_cache_state == 2) ready = 1;
        }
        pthread_mutex_unlock(&g_header_cache_mutex);
        if (phstore_install_cancel_requested()) {
            snprintf(error, 64, "install_cancelled");
            errno = ECANCELED;
            return -1;
        }
        if (fill) {
            pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.header_cache_misses++; pthread_mutex_unlock(&g_raw_diag_mutex);
            phstore_raw_http_response_t source = {.fd = -1};
            int cache_ok = open_range_uncached(package, 0, HEADER_CACHE_SIZE - 1, &source, error) == 0;
            size_t filled = 0;
            while (cache_ok && filled < HEADER_CACHE_SIZE) {
                int count = phstore_raw_http_read(&source, g_header_cache + filled, HEADER_CACHE_SIZE - filled);
                if (count <= 0) { cache_ok = 0; break; }
                filled += (size_t)count;
            }
            if (cache_ok) {
                g_header_cache_fill_connect_ms = source.connect_ms;
                g_header_cache_fill_header_ms = source.header_wait_ms;
                g_header_cache_fill_body_ms = source.body_ms;
                g_header_cache_fill_bytes = source.bytes_received;
                g_header_cache_fill_first_byte_at_ms = source.first_byte_at_ms;
            }
            phstore_raw_http_close(&source);
            pthread_mutex_lock(&g_header_cache_mutex);
            g_header_cache_state = cache_ok && filled == HEADER_CACHE_SIZE ? 2 : 0;
            pthread_cond_broadcast(&g_header_cache_condition);
            ready = g_header_cache_state == 2;
            pthread_mutex_unlock(&g_header_cache_mutex);
            if (!ready) {
                if (phstore_install_cancel_requested()) { snprintf(error, 64, "install_cancelled"); errno = ECANCELED; return -1; }
                return open_range_uncached(package, start, end, response, error);
            }
            copy_header_cache_range(start, end, response, 0, g_header_cache_fill_bytes);
            response->connect_ms = g_header_cache_fill_connect_ms;
            response->header_wait_ms = g_header_cache_fill_header_ms;
            response->body_ms = g_header_cache_fill_body_ms;
            response->first_byte_at_ms = g_header_cache_fill_first_byte_at_ms;
            return 0;
        }
        if (ready) {
            copy_header_cache_range(start, end, response, 1, 0);
            return 0;
        }
    }
    if (start < HEADER_CACHE_SIZE) {
        pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.header_cache_misses++; pthread_mutex_unlock(&g_raw_diag_mutex);
    }
    return open_range_uncached(package, start, end, response, error);
}

int phstore_raw_http_read(phstore_raw_http_response_t *response, void *buffer, size_t capacity) {
    if (response && response->cache_backed && buffer && capacity && response->remaining) {
        size_t copied = response->remaining < capacity ? (size_t)response->remaining : capacity;
        memcpy(buffer, g_header_cache + response->cache_offset, copied);
        response->cache_offset += copied;
        response->remaining -= copied;
        return (int)copied;
    }
    if (response && response->fd < 0 && response->buffered_offset < response->buffered_length && response->remaining) {
        size_t available = response->buffered_length - response->buffered_offset;
        size_t copied = available < capacity ? available : capacity;
        if (copied > response->remaining) copied = (size_t)response->remaining;
        memcpy(buffer, response->buffered + response->buffered_offset, copied);
        response->buffered_offset += copied;
        response->remaining -= copied;
        response->bytes_received += copied;
        return (int)copied;
    }
    if (!response || response->fd < 0 || !buffer || !capacity || !response->remaining) return 0;
    if (capacity > response->remaining) capacity = (size_t)response->remaining;
    size_t copied = 0;
    if (response->buffered_offset < response->buffered_length) {
        size_t available = response->buffered_length - response->buffered_offset;
        copied = available < capacity ? available : capacity;
        memcpy(buffer, response->buffered + response->buffered_offset, copied);
        response->buffered_offset += copied;
    }
    uint64_t deadline = now_ms() + RAW_IO_TIMEOUT_MS;
    while (copied < capacity) {
        if (wait_fd(response->fd, POLLIN, deadline) != 0) break;
        ssize_t count = recv(response->fd, (char *)buffer + copied, capacity - copied, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { if (!count) errno = ECONNRESET; break; }
        copied += (size_t)count;
    }
    if (copied) {
        uint64_t now = now_ms();
        if (!response->first_byte_at_ms) response->first_byte_at_ms = now;
        if (!response->body_started_at_ms) response->body_started_at_ms = now;
        response->body_ms = now - response->body_started_at_ms;
        response->remaining -= copied;
        response->bytes_received += copied;
        pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.bytes += copied; pthread_mutex_unlock(&g_raw_diag_mutex);
        return (int)copied;
    }
    int saved = errno ? errno : EIO;
    invalidate_endpoint(response->endpoint_host, response->endpoint_port);
    pthread_mutex_lock(&g_raw_diag_mutex); g_raw_diag.read_failures++; pthread_mutex_unlock(&g_raw_diag_mutex);
    raw_error("body_read_failed", saved);
    return -1;
}

void phstore_raw_http_close(phstore_raw_http_response_t *response) {
    if (!response) return;
    if (response->fd >= 0) close_tracked_socket(response->fd);
    memset(response, 0, sizeof(*response)); response->fd = -1;
}

/* Cache-only GET, sharing the proven bounded socket/header primitives above.
 * Never call raw_request/connect_http/read/cancel here: those own package
 * diagnostics, DNS/header caches and the package active-socket registry. */
void phstore_raw_image_close(phstore_raw_image_response_t *response) {
    if (response && response->fd >= 0) { close(response->fd); response->fd = -1; }
}

int phstore_raw_image_open(const char *key, uint64_t maximum, phstore_raw_image_response_t *response) {
    if (!response) { errno = EINVAL; return -1; }
    memset(response, 0, sizeof(*response)); response->fd = -1;
    response->stage = "raw_image_key";
    if (!key || strlen(key) != 32) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < 32; i++)
        if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) {
            errno = EINVAL; return -1;
        }
    response->stage = "raw_image_connect";
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(80)};
    if (inet_pton(AF_INET, PHSTORE_COVER_HOST, &address.sin_addr) != 1) { errno = EINVAL; return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    response->fd = fd;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) goto fail;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, now_ms() + RAW_CONNECT_TIMEOUT_MS) != 0) goto fail;
        int socket_error = 0; socklen_t length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) goto fail;
        if (socket_error) { errno = socket_error; goto fail; }
    }
    response->stage = "raw_image_send";
    char request[512];
    int length = snprintf(request, sizeof(request),
        "GET " PHSTORE_COVER_PATH "%s.img HTTP/1.1\r\nHost: " PHSTORE_COVER_HOST
        "\r\nUser-Agent: PHStore-Images/2.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", key);
    if (length <= 0 || (size_t)length >= sizeof(request)) { errno = EINVAL; goto fail; }
    if (send_bounded(fd, request, (size_t)length) != 0) goto fail;
    response->stage = "raw_image_headers";
    size_t header_length = 0, body_offset = 0;
    char *headers = (char *)response->buffered;
    if (read_response_headers(fd, headers, &header_length, &body_offset) != 0) goto fail;
    if (sscanf(headers, "HTTP/%*u.%*u %d", &response->status) != 1) { errno = EPROTO; goto fail; }
    if (response->status != 200) { errno = response->status == 404 ? ENOENT : EPROTO; goto fail; }
    char value[96];
    if (header_value(headers, "Transfer-Encoding", value, sizeof(value))) { errno = EPROTO; goto fail; }
    if (header_value(headers, "Content-Encoding", value, sizeof(value)) && strcasecmp(value, "identity")) {
        errno = EPROTO; goto fail;
    }
    if (!header_value(headers, "Content-Length", value, sizeof(value)) ||
        !parse_u64_text(value, &response->content_length)) { errno = EPROTO; goto fail; }
    if (!response->content_length || response->content_length > maximum) { errno = EFBIG; goto fail; }
    response->buffered_length = header_length - body_offset;
    if (response->buffered_length > response->content_length) { errno = EPROTO; goto fail; }
    memmove(response->buffered, response->buffered + body_offset, response->buffered_length);
    response->remaining = response->content_length;
    response->stage = "raw_image_body";
    return 0;
fail: {
    int error = errno ? errno : EIO;
    phstore_raw_image_close(response); errno = error; return -1;
}
}

int phstore_raw_image_read(phstore_raw_image_response_t *response, void *buffer, size_t capacity) {
    if (!response || !buffer || !capacity) { errno = EINVAL; return -1; }
    if (!response->remaining) return 0;
    if (response->fd < 0) { errno = EBADF; return -1; }
    if (capacity > response->remaining) capacity = (size_t)response->remaining;
    if (capacity > INT32_MAX) capacity = INT32_MAX;
    size_t buffered = response->buffered_length - response->buffered_offset;
    if (buffered) {
        if (buffered > capacity) buffered = capacity;
        memcpy(buffer, response->buffered + response->buffered_offset, buffered);
        response->buffered_offset += buffered; response->remaining -= buffered;
        return (int)buffered;
    }
    uint64_t deadline = now_ms() + RAW_IO_TIMEOUT_MS;
    for (;;) {
        if (wait_fd(response->fd, POLLIN, deadline) != 0) return -1;
        ssize_t count = recv(response->fd, buffer, capacity, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { if (!count) errno = ECONNRESET; return -1; }
        response->remaining -= (uint64_t)count;
        return (int)count;
    }
}
