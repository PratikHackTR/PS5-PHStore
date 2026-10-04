/* Native SceHttp2 upstream transport shared by source_group and direct_http. */
#include "phstore_upstream.h"
#include "phstore_config.h"
#include "phstore_url.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define UPSTREAM_HEADER_MAX 16384
#define UPSTREAM_REDIRECT_MAX 5

static int header_value(const char *headers, size_t length, const char *name,
                        char *output, size_t capacity);

extern int sceNetPoolCreate(const char *, int, int);
extern int sceNetPoolDestroy(int);
extern int sceSslInit(size_t);
extern int sceSslTerm(int);
extern int sceHttp2Init(int, int, size_t, int);
extern int sceHttp2Term(int);
extern int sceHttp2CreateTemplate(int, const char *, int, int);
extern int sceHttp2DeleteTemplate(int);
extern int sceHttp2SetAutoRedirect(int, int);
extern int sceHttp2SetResolveTimeOut(int, unsigned int);
extern int sceHttp2SetConnectTimeOut(int, unsigned int);
extern int sceHttp2SetSendTimeOut(int, unsigned int);
extern int sceHttp2SetRecvTimeOut(int, unsigned int);
extern int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
extern int sceHttp2DeleteRequest(int);
extern int sceHttp2AddRequestHeader(int, const char *, const char *, unsigned int);
extern int sceHttp2SendRequest(int, const void *, size_t);
extern int sceHttp2GetStatusCode(int, int *);
extern int sceHttp2GetAllResponseHeaders(int, char **, unsigned int *);
extern int sceHttp2GetResponseContentLength(int, uint64_t *);
extern int sceHttp2ReadData(int, void *, size_t);

static pthread_mutex_t g_upstream_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static phstore_upstream_diagnostics_t g_diag;
static int g_net_pool = -1;
static int g_ssl_context = -1;
static int g_http_context = -1;
static int g_http_template = -1;
static int g_initialized;

static void native_stage(const char *function) {
    struct timespec now;
    uint64_t milliseconds = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        milliseconds = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    pthread_mutex_lock(&g_diag_mutex);
    snprintf(g_diag.stage, sizeof(g_diag.stage), "%s", function);
    pthread_mutex_unlock(&g_diag_mutex);
    phstore_install_set_upstream_stage(function);
    fprintf(stderr, "[PHSTORE/UPSTREAM] t=%llu stage=%s started\n", (unsigned long long)milliseconds, function);
}

static void set_error(char error[64], const char *value) {
    if (error) snprintf(error, 64, "%s", value);
}

static void diagnose_url(const char *url) {
    if (!url) return;
    pthread_mutex_lock(&g_diag_mutex);
    size_t scheme = !strncasecmp(url, "https://", 8) ? 8 : 7;
    snprintf(g_diag.scheme, sizeof(g_diag.scheme), "%s", scheme == 8 ? "https" : "http");
    const char *authority = url + scheme;
    const char *end = authority + strcspn(authority, "/?");
    const char *host = authority;
    const char *port = NULL;
    if (host < end && *host == '[') {
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (close) { host++; if (close + 1 < end && close[1] == ':') port = close + 2; end = close; }
    } else {
        for (const char *p = host; p < end; p++) if (*p == ':') { port = p + 1; end = p; break; }
    }
    size_t host_length = (size_t)(end - host);
    if (host_length >= sizeof(g_diag.host)) host_length = sizeof(g_diag.host) - 1;
    memcpy(g_diag.host, host, host_length); g_diag.host[host_length] = '\0';
    g_diag.port = scheme == 8 ? 443 : 80;
    if (port) {
        unsigned value = 0;
        while (*port >= '0' && *port <= '9') { value = value * 10 + (unsigned)(*port++ - '0'); if (value > 65535) break; }
        if (value > 0 && value <= 65535) g_diag.port = (uint16_t)value;
    }
    pthread_mutex_unlock(&g_diag_mutex);
}

static void diag_native(const char *function, int result, const char *url) {
    pthread_mutex_lock(&g_diag_mutex);
    g_diag.native_result = result;
    snprintf(g_diag.native_function, sizeof(g_diag.native_function), "%s", function);
    snprintf(g_diag.stage, sizeof(g_diag.stage), "%s", function);
    pthread_mutex_unlock(&g_diag_mutex);
    phstore_install_set_upstream_stage(function);
    diagnose_url(url);
    phstore_upstream_diagnostics_t snapshot;
    phstore_upstream_get_diagnostics(&snapshot);
    fprintf(stderr, "[PHSTORE/UPSTREAM] %s rc=0x%08X (%d) scheme=%s host=%s port=%u\n",
            function, (unsigned)result, result, snapshot.scheme, snapshot.host, snapshot.port);
}

static void diag_status(int status, const char *headers, size_t length) {
    pthread_mutex_lock(&g_diag_mutex);
    g_diag.status_code = status;
    char value[96];
    if (headers && header_value(headers, length, "Content-Range", value, sizeof(value)))
        snprintf(g_diag.last_content_range, sizeof(g_diag.last_content_range), "%s", value);
    pthread_mutex_unlock(&g_diag_mutex);
}

void phstore_upstream_get_diagnostics(phstore_upstream_diagnostics_t *diagnostics) {
    if (!diagnostics) return;
    pthread_mutex_lock(&g_diag_mutex);
    *diagnostics = g_diag;
    pthread_mutex_unlock(&g_diag_mutex);
    uint64_t bytes = 0, head_size = 0; uint32_t requests = 0, heads = 0, redirects = 0, failures = 0;
    int status = 0, head_status = 0, range_status = 0;
    char stage[48] = {0}, range[96] = {0}; int32_t native_result = 0;
    phstore_raw_http_get_diagnostics(&bytes, &requests, &heads, &redirects, &failures,
        &status, &head_status, &head_size, &range_status, stage, range, &native_result);
    if (!strcmp(diagnostics->scheme, "http")) {
        diagnostics->bytes_received = bytes; diagnostics->request_count = requests;
        diagnostics->head_count = heads; diagnostics->redirect_count = redirects;
        diagnostics->read_failures = failures; diagnostics->status_code = status;
        diagnostics->head_status_code = head_status; diagnostics->head_content_length = head_size;
        diagnostics->range_probe_status_code = range_status;
        diagnostics->native_result = native_result;
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "%s", stage);
        snprintf(diagnostics->native_function, sizeof(diagnostics->native_function), "%s", stage);
        if (range[0]) snprintf(diagnostics->last_content_range, sizeof(diagnostics->last_content_range), "%s", range);
    }
}

void phstore_upstream_reset_diagnostics(const char *url) {
    pthread_mutex_lock(&g_diag_mutex);
    memset(&g_diag, 0, sizeof(g_diag));
    pthread_mutex_unlock(&g_diag_mutex);
    phstore_raw_http_reset_diagnostics();
    diagnose_url(url);
}

static int header_value(const char *headers, size_t length, const char *name,
                        char *output, size_t capacity) {
    size_t name_length = strlen(name);
    const char *line = headers;
    const char *end = headers + length;
    while (line < end) {
        const char *line_end = line;
        while (line_end < end && *line_end != '\n') line_end++;
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && (size_t)(colon - line) == name_length) {
            size_t i = 0;
            for (; i < name_length; i++)
                if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) break;
            if (i == name_length) {
                const char *value = colon + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) value++;
                const char *value_end = line_end;
                while (value_end > value && (value_end[-1] == '\r' || value_end[-1] == ' ' || value_end[-1] == '\t')) value_end--;
                size_t value_length = (size_t)(value_end - value);
                if (!capacity || value_length >= capacity) return 0;
                memcpy(output, value, value_length);
                output[value_length] = '\0';
                return 1;
            }
        }
        line = line_end < end ? line_end + 1 : end;
    }
    return 0;
}

static int parse_u64(const char *text, const char **end, uint64_t *value) {
    if (!text || *text < '0' || *text > '9') return 0;
    uint64_t result = 0;
    const char *cursor = text;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned digit = (unsigned)(*cursor - '0');
        if (result > (UINT64_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
        cursor++;
    }
    if (end) *end = cursor;
    *value = result;
    return 1;
}

static int parse_content_range(const char *headers, size_t length,
                               uint64_t *start, uint64_t *end, uint64_t *total) {
    char value[160];
    if (!header_value(headers, length, "Content-Range", value, sizeof(value))) return 0;
    if (strncmp(value, "bytes ", 6) != 0) return 0;
    const char *cursor = value + 6, *next;
    if (!parse_u64(cursor, &next, start) || *next++ != '-' ||
        !parse_u64(next, &cursor, end) || *cursor++ != '/' ||
        !parse_u64(cursor, &next, total) || *next != '\0') return 0;
    return *start <= *end && *end < *total;
}

static int request_once(const char *url, const char *method, int use_range,
                        uint64_t start, uint64_t end, int *request_id, int *status,
                        uint64_t *content_length, char headers[UPSTREAM_HEADER_MAX],
                        size_t *headers_length, char error[64]) {
    diagnose_url(url);
    pthread_mutex_lock(&g_diag_mutex);
    g_diag.request_count++;
    if (!strcmp(method, "HEAD")) g_diag.head_count++;
    pthread_mutex_unlock(&g_diag_mutex);
    native_stage("sceHttp2CreateRequestWithURL");
    int request = sceHttp2CreateRequestWithURL(g_http_template, method, url, 0);
    if (request < 0) { diag_native("sceHttp2CreateRequestWithURL", request, url); set_error(error, strncmp(url, "https://", 8) == 0 ? "upstream_tls_failed" : "upstream_connect_failed"); return -1; }
    if (use_range) {
        char range[96];
        snprintf(range, sizeof(range), "bytes=%llu-%llu", (unsigned long long)start, (unsigned long long)end);
        native_stage("sceHttp2AddRequestHeader(Range)");
        int added = sceHttp2AddRequestHeader(request, "Range", range, 0);
        if (added < 0) { diag_native("sceHttp2AddRequestHeader(Range)", added, url); sceHttp2DeleteRequest(request); set_error(error, "upstream_range_invalid"); return -1; }
    }
    native_stage("sceHttp2AddRequestHeader(Accept-Encoding)");
    int encoding = sceHttp2AddRequestHeader(request, "Accept-Encoding", "identity", 0);
    if (encoding < 0) { diag_native("sceHttp2AddRequestHeader(Accept-Encoding)", encoding, url); sceHttp2DeleteRequest(request); set_error(error, "upstream_connect_failed"); return -1; }
    native_stage("sceHttp2SendRequest");
    int sent = sceHttp2SendRequest(request, NULL, 0);
    if (sent < 0) {
        diag_native("sceHttp2SendRequest", sent, url);
        sceHttp2DeleteRequest(request);
        set_error(error, strncmp(url, "https://", 8) == 0 ? "upstream_tls_failed" : "upstream_connect_failed");
        return -1;
    }
    native_stage("sceHttp2GetStatusCode");
    int status_result = sceHttp2GetStatusCode(request, status);
    if (status_result < 0) {
        diag_native("sceHttp2GetStatusCode", status_result, url);
        sceHttp2DeleteRequest(request);
        set_error(error, !strncasecmp(url, "https://", 8) ? "upstream_tls_failed" : "upstream_http_error");
        return -1;
    }
    pthread_mutex_lock(&g_diag_mutex); g_diag.status_code = *status; pthread_mutex_unlock(&g_diag_mutex);
    char *native_headers = NULL;
    unsigned int native_length = 0;
    native_stage("sceHttp2GetAllResponseHeaders");
    int headers_result = sceHttp2GetAllResponseHeaders(request, &native_headers, &native_length);
    if (headers_result < 0 || !native_headers || native_length > UPSTREAM_HEADER_MAX - 1) {
        diag_native("sceHttp2GetAllResponseHeaders", headers_result < 0 ? headers_result : -EIO, url);
        sceHttp2DeleteRequest(request);
        set_error(error, headers_result < 0 && !strncasecmp(url, "https://", 8)
            ? "upstream_tls_failed" : "upstream_http_error");
        return -1;
    }
    memcpy(headers, native_headers, native_length);
    headers[native_length] = '\0';
    *headers_length = native_length;
    diag_status(*status, headers, native_length);
    *content_length = 0;
    native_stage("sceHttp2GetResponseContentLength");
    int length_result = sceHttp2GetResponseContentLength(request, content_length);
    if (length_result < 0) {
        diag_native("sceHttp2GetResponseContentLength", length_result, url);
        char content_length_text[32];
        if (header_value(headers, native_length, "Content-Length", content_length_text, sizeof(content_length_text))) {
            const char *tail;
            if (!parse_u64(content_length_text, &tail, content_length) || *tail) *content_length = 0;
        }
    }
    *request_id = request;
    return 0;
}

static int is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static int request_follow_redirects(const char *initial_url, const char *method, int use_range,
                                    uint64_t start, uint64_t end, int *request_id, int *status,
                                    uint64_t *content_length, char headers[UPSTREAM_HEADER_MAX],
                                    size_t *headers_length, char final_url[PHSTORE_DOWNLOAD_URL_MAX],
                                    char error[64]) {
    if (!phstore_url_validate(initial_url)) { set_error(error, "direct_url_invalid"); return -1; }
    snprintf(final_url, PHSTORE_DOWNLOAD_URL_MAX, "%s", initial_url);
    for (unsigned redirects = 0; redirects <= UPSTREAM_REDIRECT_MAX; redirects++) {
        if (request_once(final_url, method, use_range, start, end, request_id, status,
                         content_length, headers, headers_length, error) != 0) return -1;
        if (!is_redirect(*status)) return 0;
        char location[PHSTORE_DOWNLOAD_URL_MAX];
        if (!header_value(headers, *headers_length, "Location", location, sizeof(location))) {
            sceHttp2DeleteRequest(*request_id); set_error(error, "upstream_http_error"); return -1;
        }
        if (redirects == UPSTREAM_REDIRECT_MAX) {
            sceHttp2DeleteRequest(*request_id); set_error(error, "redirect_limit"); return -1;
        }
        pthread_mutex_lock(&g_diag_mutex); g_diag.redirect_count++; pthread_mutex_unlock(&g_diag_mutex);
        char resolved[PHSTORE_DOWNLOAD_URL_MAX];
        if (!phstore_url_resolve_redirect(final_url, location, resolved, sizeof(resolved))) {
            sceHttp2DeleteRequest(*request_id);
            set_error(error, phstore_url_validate(location) ? "direct_url_invalid" :
                      ((strncmp(location, "http://", 7) && strncmp(location, "https://", 8)) ? "unsupported_scheme" : "direct_url_invalid"));
            return -1;
        }
        sceHttp2DeleteRequest(*request_id);
        snprintf(final_url, PHSTORE_DOWNLOAD_URL_MAX, "%s", resolved);
    }
    set_error(error, "redirect_limit");
    return -1;
}

int phstore_upstream_init(char error[64]) {
    if (error) error[0] = '\0';
    pthread_mutex_lock(&g_upstream_mutex);
    if (g_initialized) { pthread_mutex_unlock(&g_upstream_mutex); return 0; }
    native_stage("sceNetPoolCreate");
    g_net_pool = sceNetPoolCreate("phstore-upstream", 32 * 1024, 0);
    if (g_net_pool < 0) { diag_native("sceNetPoolCreate", g_net_pool, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    native_stage("sceSslInit");
    g_ssl_context = sceSslInit(256 * 1024);
    if (g_ssl_context < 0) { diag_native("sceSslInit", g_ssl_context, NULL); set_error(error, "upstream_tls_failed"); goto fail; }
    native_stage("sceHttp2Init");
    g_http_context = sceHttp2Init(g_net_pool, g_ssl_context, 256 * 1024, 1);
    if (g_http_context < 0) { diag_native("sceHttp2Init", g_http_context, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    native_stage("sceHttp2CreateTemplate");
    g_http_template = sceHttp2CreateTemplate(g_http_context, "PHStore/1.0", 3, 1);
    if (g_http_template < 0) {
        diag_native("sceHttp2CreateTemplate", g_http_template, NULL);
        set_error(error, "upstream_connect_failed"); goto fail;
    }
    native_stage("sceHttp2SetAutoRedirect");
    int redirect_result = sceHttp2SetAutoRedirect(g_http_template, 0);
    if (redirect_result < 0) { diag_native("sceHttp2SetAutoRedirect", redirect_result, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    native_stage("sceHttp2SetResolveTimeOut");
    int timeout_result = sceHttp2SetResolveTimeOut(g_http_template, 15000000u);
    if (timeout_result < 0) { diag_native("sceHttp2SetResolveTimeOut", timeout_result, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    native_stage("sceHttp2SetConnectTimeOut");
    timeout_result = sceHttp2SetConnectTimeOut(g_http_template, 15000000u);
    if (timeout_result < 0) { diag_native("sceHttp2SetConnectTimeOut", timeout_result, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    native_stage("sceHttp2SetSendTimeOut");
    timeout_result = sceHttp2SetSendTimeOut(g_http_template, 15000000u);
    if (timeout_result < 0) { diag_native("sceHttp2SetSendTimeOut", timeout_result, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    native_stage("sceHttp2SetRecvTimeOut");
    timeout_result = sceHttp2SetRecvTimeOut(g_http_template, 15000000u);
    if (timeout_result < 0) { diag_native("sceHttp2SetRecvTimeOut", timeout_result, NULL); set_error(error, "upstream_connect_failed"); goto fail; }
    g_initialized = 1;
    pthread_mutex_unlock(&g_upstream_mutex);
    return 0;
fail:
    if (g_http_template >= 0) { sceHttp2DeleteTemplate(g_http_template); g_http_template = -1; }
    if (g_http_context >= 0) { sceHttp2Term(g_http_context); g_http_context = -1; }
    if (g_ssl_context >= 0) { sceSslTerm(g_ssl_context); g_ssl_context = -1; }
    if (g_net_pool >= 0) { sceNetPoolDestroy(g_net_pool); g_net_pool = -1; }
    pthread_mutex_unlock(&g_upstream_mutex);
    return -1;
}

void phstore_upstream_term(void) {
    pthread_mutex_lock(&g_upstream_mutex);
    if (g_http_template >= 0) sceHttp2DeleteTemplate(g_http_template);
    if (g_http_context >= 0) sceHttp2Term(g_http_context);
    if (g_ssl_context >= 0) sceSslTerm(g_ssl_context);
    if (g_net_pool >= 0) sceNetPoolDestroy(g_net_pool);
    g_http_template = g_http_context = g_ssl_context = g_net_pool = -1;
    g_initialized = 0;
    pthread_mutex_unlock(&g_upstream_mutex);
}

static int parse_range_response(const char *headers, size_t length, uint64_t requested_start,
                                uint64_t requested_end, uint64_t expected_total,
                                uint64_t *response_length) {
    uint64_t range_start, range_end, range_total;
    if (!parse_content_range(headers, length, &range_start, &range_end, &range_total) ||
        range_start != requested_start || range_end != requested_end || range_total != expected_total) return 0;
    uint64_t range_length = requested_end - requested_start + 1;
    char length_text[32];
    if (header_value(headers, length, "Content-Length", length_text, sizeof(length_text))) {
        const char *tail;
        if (!parse_u64(length_text, &tail, response_length) || *tail || *response_length != range_length) return 0;
    } else *response_length = range_length;
    return 1;
}

int phstore_upstream_probe(const phstore_package_info_t *package, char error[64]) {
    if (!package || !package->download_url[0] || !package->size_bytes) { set_error(error, "package_source_missing"); return -1; }
    diagnose_url(package->download_url);
    if (!strncasecmp(package->download_url, "http://", 7)) {
        set_error(error, "");
        native_stage("probe_started");
        return phstore_raw_http_probe(package, error);
    }
    if (phstore_upstream_init(error) != 0) return -1;
    pthread_mutex_lock(&g_upstream_mutex);
    int request = -1, status = 0;
    uint64_t content_length = 0;
    char headers[UPSTREAM_HEADER_MAX], final_url[PHSTORE_DOWNLOAD_URL_MAX];
    size_t headers_length = 0;
    native_stage("HEAD_started");
    if (request_follow_redirects(package->download_url, "HEAD", 0, 0, 0, &request, &status,
            &content_length, headers, &headers_length, final_url, error) != 0) goto failed;
    fprintf(stderr, "[PHSTORE/UPSTREAM] HEAD_completed status=%d content_length=%llu\n",
            status, (unsigned long long)content_length);
    pthread_mutex_lock(&g_diag_mutex); g_diag.head_status_code = status; g_diag.head_content_length = content_length; pthread_mutex_unlock(&g_diag_mutex);
    sceHttp2DeleteRequest(request); request = -1;
    if (status >= 200 && status < 300 && content_length && content_length != package->size_bytes) {
        /* Range response below is authoritative when a server reports unsuitable HEAD metadata. */
    } else if (status < 200 || status >= 400) {
        /* Some sources reject HEAD but still serve correct byte ranges. */
        if (status != 403 && status != 405 && status != 501) {
            set_error(error, "upstream_http_error"); goto failed;
        }
    }

    native_stage("range_probe_started");
    if (request_follow_redirects(package->download_url, "GET", 1, 0, 0, &request, &status,
            &content_length, headers, &headers_length, final_url, error) != 0) goto failed;
    fprintf(stderr, "[PHSTORE/UPSTREAM] range_probe_completed status=%d content_range=%s\n",
            status, g_diag.last_content_range);
    pthread_mutex_lock(&g_diag_mutex); g_diag.range_probe_status_code = status; pthread_mutex_unlock(&g_diag_mutex);
    if (status != 206) { set_error(error, "upstream_range_unsupported"); goto failed; }
    uint64_t range_length = 0;
    if (!parse_range_response(headers, headers_length, 0, 0, package->size_bytes, &range_length) || range_length != 1) {
        set_error(error, "upstream_range_invalid"); goto failed;
    }
    unsigned char byte;
    int read_result = sceHttp2ReadData(request, &byte, 1);
    if (read_result != 1) {
        diag_native("sceHttp2ReadData(probe)", read_result, final_url);
        set_error(error, read_result < 0 && !strncasecmp(final_url, "https://", 8)
            ? "upstream_tls_failed" : "upstream_range_invalid"); goto failed;
    }
    sceHttp2DeleteRequest(request);
    pthread_mutex_unlock(&g_upstream_mutex);
    return 0;
failed:
    if (request >= 0) sceHttp2DeleteRequest(request);
    pthread_mutex_unlock(&g_upstream_mutex);
    return -1;
}

int phstore_upstream_open_range(const phstore_package_info_t *package,
                                uint64_t start, uint64_t end,
                                phstore_upstream_response_t *response, char error[64]) {
    if (!package || !response || start > end || end >= package->size_bytes) {
        set_error(error, "upstream_range_invalid"); return -1;
    }
    memset(response, 0, sizeof(*response)); response->request_id = -1; response->raw.fd = -1;
    if (!strncasecmp(package->download_url, "http://", 7)) {
        set_error(error, "");
        if (phstore_raw_http_open_range(package, start, end, &response->raw, error) != 0) return -1;
        response->transport_type = 1;
        response->requested_start = start; response->requested_end = end;
        response->total_size = package->size_bytes; response->response_length = end - start + 1;
        return 0;
    }
    if (phstore_upstream_init(error) != 0) return -1;
    pthread_mutex_lock(&g_upstream_mutex);
    response->transport_type = 0;
    int status = 0;
    char headers[UPSTREAM_HEADER_MAX], final_url[PHSTORE_DOWNLOAD_URL_MAX];
    size_t headers_length = 0;
    uint64_t content_length = 0;
    if (request_follow_redirects(package->download_url, "GET", 1, start, end,
            &response->request_id, &status, &content_length, headers, &headers_length,
            final_url, error) != 0) goto failed;
    if (status != 206) { set_error(error, "upstream_range_unsupported"); goto failed; }
    response->response_length = content_length;
    if (!parse_range_response(headers, headers_length, start, end, package->size_bytes,
                              &response->response_length)) {
        set_error(error, "upstream_range_invalid"); goto failed;
    }
    response->requested_start = start;
    response->requested_end = end;
    response->total_size = package->size_bytes;
    return 0;
failed:
    if (response->request_id >= 0) sceHttp2DeleteRequest(response->request_id);
    response->request_id = -1;
    pthread_mutex_unlock(&g_upstream_mutex);
    return -1;
}

int phstore_upstream_read(phstore_upstream_response_t *response, void *buffer, size_t capacity) {
    if (!response || !buffer || !capacity) return -1;
    if (response->transport_type == 1) return phstore_raw_http_read(&response->raw, buffer, capacity);
    if (response->request_id < 0) return -1;
    native_stage("sceHttp2ReadData");
    int result = sceHttp2ReadData(response->request_id, buffer, capacity);
    pthread_mutex_lock(&g_diag_mutex);
    if (result > 0) g_diag.bytes_received += (uint64_t)result;
    else if (result <= 0) g_diag.read_failures++;
    pthread_mutex_unlock(&g_diag_mutex);
    if (result <= 0) diag_native("sceHttp2ReadData", result, NULL);
    return result;
}

void phstore_upstream_close(phstore_upstream_response_t *response) {
    if (!response) return;
    if (response->transport_type == 1) {
        phstore_raw_http_close(&response->raw);
        response->transport_type = 0;
        return;
    }
    if (response->request_id < 0) return;
    sceHttp2DeleteRequest(response->request_id);
    response->request_id = -1;
    pthread_mutex_unlock(&g_upstream_mutex);
}
