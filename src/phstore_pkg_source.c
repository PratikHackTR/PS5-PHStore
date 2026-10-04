#include "phstore_pkg_source.h"
#include "phstore_gdrive.h"
#include <sys/stat.h>

#include "phstore_install.h"
#include "phstore_raw_http.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define open _open
#define write _write
#define fsync _commit
#define close _close
#else
#include <unistd.h>
#endif

/* Keep bounded sequential Range responses alive across relay reads. No
 * prefetch thread and no full-package RAM/disk buffer. Small random reads keep
 * their exact ranges/header cache. Each occupied lane has one exclusive reader. */
#define SOURCE_LANES 4
#define SOURCE_WINDOW_BYTES (32u * 1024u * 1024u)
#define SOURCE_BULK_MIN (256u * 1024u)
typedef struct {
    phstore_raw_http_response_t response;
    uint64_t next_offset;
    int busy;
} source_lane_t;
typedef struct {
    int local_fd;
    phstore_package_info_t package;
    pthread_mutex_t lanes_mutex;
    source_lane_t lanes[SOURCE_LANES];
} phstore_pkg_source_handle_t;

static pthread_mutex_t g_source_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void source_log(const char *event, const char *package_id, uint64_t offset,
                       size_t length, const char *detail) {
    pthread_mutex_lock(&g_source_log_mutex);
    int fd = open("/data/phstore2/logs/source-latest.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        char line[384];
        int count = snprintf(line, sizeof(line), "%s package_id=%s offset=%llu length=%zu detail=%s\n",
            event, package_id ? package_id : "-", (unsigned long long)offset, length, detail ? detail : "-");
        if (count > 0 && (size_t)count < sizeof(line)) { (void)write(fd, line, (size_t)count); (void)fsync(fd); }
        close(fd);
    }
    pthread_mutex_unlock(&g_source_log_mutex);
}

int phstore_pkg_source_open_uri(const char *uri, void **handle,
                               uint64_t *size, char *filename,
                               size_t filename_capacity) {
    static const char prefix[] = "phstore://";
    if (!uri || strncmp(uri, prefix, sizeof(prefix) - 1) != 0 || !handle ||
        !size || !filename || filename_capacity == 0) return -1;
    *handle = NULL;
    *size = 0;
    filename[0] = '\0';
    const char *package_id = uri + sizeof(prefix) - 1;
    if (!*package_id || strchr(package_id, '/') || strchr(package_id, '?') ||
        strchr(package_id, '#')) return -1;

    phstore_install_detailed_log("VIRTUAL_SOURCE_OPEN_BEGIN", package_id);
    phstore_pkg_source_handle_t *source = calloc(1, sizeof(*source));
    if (!source) return -1;
    source->local_fd=-1;
    char error[64] = {0};
    if (!phstore_catalog_find_install_package(package_id, &source->package, error) ||
        phstore_catalog_resolve_package_source(&source->package, error) != 0 ||
        source->package.size_bytes == 0) {
        free(source);
        errno = ENOENT;
        phstore_install_detailed_log("VIRTUAL_SOURCE_OPEN_FAILED", error[0] ? error : "package source lookup failed");
        return -1;
    }
    if(!strcmp(source->package.source_type,"google_drive_public")&&!strcmp(source->package.action_type,"install_package")) {
        char path[512];struct stat st;
        if(phstore_gdrive_destination(&source->package,path,sizeof(path)) || (source->local_fd=open(path,O_RDONLY
#if defined(_WIN32)
            | _O_BINARY
#endif
            ))<0 ||
           fstat(source->local_fd,&st) || (uint64_t)st.st_size!=source->package.size_bytes) {
            if(source->local_fd>=0)close(source->local_fd);free(source);errno=EIO;return -1;
        }
        if(pthread_mutex_init(&source->lanes_mutex,NULL)){close(source->local_fd);free(source);return -1;}
        *size=source->package.size_bytes;snprintf(filename,filename_capacity,"%s",source->package.filename);*handle=source;
        phstore_install_detailed_log("NATIVE_DRIVE_LOCAL_SOURCE",path);return 0;
    }
    if (strncmp(source->package.download_url, "http://", 7) != 0) {
        free(source);
        errno = EPROTONOSUPPORT;
        phstore_install_detailed_log("VIRTUAL_SOURCE_SCHEME_REJECTED", "expected http");
        return -1;
    }
    int mutex_result = pthread_mutex_init(&source->lanes_mutex, NULL);
    if (mutex_result != 0) { free(source); errno = mutex_result; return -1; }
    for (int i = 0; i < SOURCE_LANES; i++) source->lanes[i].response.fd = -1;
    phstore_raw_http_source_begin(&source->package);
    source_log("OPEN", source->package.package_id, 0, 0, "random-access HTTP source ready");
    char detail[160];
    snprintf(detail, sizeof(detail), "size=%llu source_type=%s", (unsigned long long)source->package.size_bytes,
        source->package.source_type);
    phstore_install_detailed_log("VIRTUAL_SOURCE_OPEN_DONE", detail);
    *size = source->package.size_bytes;
    snprintf(filename, filename_capacity, "%s", source->package.filename);
    *handle = source;
    return 0;
}

int64_t phstore_pkg_source_read_at(void *handle, uint64_t offset,
                                  void *buffer, size_t length) {
    phstore_pkg_source_handle_t *source = handle;
    if (!source || !buffer || length == 0 || offset >= source->package.size_bytes ||
        (uint64_t)length > source->package.size_bytes - offset) {
        errno = EINVAL;
        return -1;
    }
    if (phstore_install_cancel_requested()) { errno = ECANCELED; return -1; }
    if(source->local_fd>=0) {
        size_t done=0;while(done<length) {
#if defined(_WIN32)
            pthread_mutex_lock(&source->lanes_mutex);
            int64_t n=_lseeki64(source->local_fd,(__int64)(offset+done),SEEK_SET)<0?-1:_read(source->local_fd,(unsigned char *)buffer+done,(unsigned int)(length-done));
            pthread_mutex_unlock(&source->lanes_mutex);
#else
            int64_t n=pread(source->local_fd,(unsigned char *)buffer+done,length-done,(off_t)(offset+done));
#endif
            if(n<0&&errno==EINTR)continue;if(n<=0)return -1;done+=(size_t)n;
        }
        return (int64_t)done;
    }
    source_lane_t *lane = NULL;
    if (length >= SOURCE_BULK_MIN) {
        pthread_mutex_lock(&source->lanes_mutex);
        /* Prefer the response already positioned at this exact byte. */
        for (int i = 0; i < SOURCE_LANES; i++) {
            source_lane_t *candidate = &source->lanes[i];
            if (!candidate->busy && candidate->next_offset == offset &&
                candidate->response.remaining >= length) { lane = candidate; break; }
        }
        if (!lane) {
            for (int i = 0; i < SOURCE_LANES; i++) {
                source_lane_t *candidate = &source->lanes[i];
                if (!candidate->busy && (!lane || !candidate->response.remaining)) lane = candidate;
                if (lane && !lane->response.remaining) break;
            }
        }
        if (lane) lane->busy = 1;
        pthread_mutex_unlock(&source->lanes_mutex);
    }
    /* If all lanes are busy, use an independent exact range, never block an
     * unrelated reader behind network I/O or share its mutable response. */
    phstore_raw_http_response_t local = {.fd = -1};
    phstore_raw_http_response_t *response = lane ? &lane->response : &local;
    int reused = lane && lane->next_offset == offset && response->remaining >= length;
    char error[64] = {0}, detail[512];
    size_t done = 0;
    int retries = 0;
    if (!reused) {
        phstore_raw_http_close(response);
        uint64_t span = length;
        if (lane && span < SOURCE_WINDOW_BYTES) span = SOURCE_WINDOW_BYTES;
        if (span > source->package.size_bytes - offset) span = source->package.size_bytes - offset;
        snprintf(detail, sizeof(detail), "offset=%llu requested=%zu window=%llu",
            (unsigned long long)offset, length, (unsigned long long)span);
        phstore_install_detailed_log("SOURCE_WINDOW_BEGIN", detail);
        if (phstore_raw_http_open_range(&source->package, offset, offset + span - 1, response, error) != 0) {
            /* Some origins reject large ranges. Preserve exact-range support. */
            phstore_raw_http_close(response);
            if (span == length || phstore_install_cancel_requested() ||
                phstore_raw_http_open_range(&source->package, offset, offset + length - 1, response, error) != 0)
                goto failed;
            phstore_install_detailed_log("SOURCE_WINDOW_EXACT_FALLBACK", error);
        }
        snprintf(detail, sizeof(detail), "offset=%llu bytes=%llu host=%s port=%s connect_ms=%llu headers_ms=%llu",
            (unsigned long long)offset, (unsigned long long)response->remaining,
            response->endpoint_host, response->endpoint_port,
            (unsigned long long)response->connect_ms, (unsigned long long)response->header_wait_ms);
        phstore_install_detailed_log("SOURCE_WINDOW_OPEN_DONE", detail);
    }
    while (done < length) {
        int count = phstore_raw_http_read(response, (unsigned char *)buffer + done, length - done);
        if (count <= 0 || (size_t)count > length - done) {
            phstore_raw_http_close(response);
            /* A retained connection may have been closed by the origin while
             * idle. Resume only the missing bytes, once; never replay bytes. */
            if (retries++ == 0 && !phstore_install_cancel_requested() &&
                phstore_raw_http_open_range(&source->package, offset + done, offset + length - 1, response, error) == 0) {
                phstore_install_detailed_log("SOURCE_WINDOW_RESUME", "exact missing bytes after body failure");
                continue;
            }
            goto failed;
        }
        done += (size_t)count;
    }
    snprintf(detail, sizeof(detail), "offset=%llu bytes=%zu reused=%d remaining=%llu",
        (unsigned long long)offset, done, reused, (unsigned long long)response->remaining);
    phstore_install_detailed_log("SOURCE_RANGE_READ_DONE", detail);
    if (!lane || !response->remaining) phstore_raw_http_close(response);
    if (lane) {
        pthread_mutex_lock(&source->lanes_mutex);
        lane->next_offset = offset + done;
        lane->busy = 0;
        pthread_mutex_unlock(&source->lanes_mutex);
    }
    return (int64_t)done;
failed:
    phstore_raw_http_close(response);
    source_log("RANGE_FAILED", source->package.package_id, offset, length,
        error[0] ? error : "short or failed range body");
    phstore_install_detailed_log("SOURCE_RANGE_FAILED", error[0] ? error : "short or failed range body");
    if (lane) {
        pthread_mutex_lock(&source->lanes_mutex);
        lane->next_offset = 0;
        lane->busy = 0;
        pthread_mutex_unlock(&source->lanes_mutex);
    }
    errno = phstore_install_cancel_requested() ? ECANCELED : EIO;
    return -1;
}

void phstore_pkg_source_close(void *handle) {
    phstore_pkg_source_handle_t *source = handle;
    /* Upstream stream references are drained before source destruction. */
    if(source&&source->local_fd>=0){close(source->local_fd);pthread_mutex_destroy(&source->lanes_mutex);free(source);return;}
    if (source) {
        for (int i = 0; i < SOURCE_LANES; i++) phstore_raw_http_close(&source->lanes[i].response);
        pthread_mutex_destroy(&source->lanes_mutex);
        source_log("CLOSE", source->package.package_id, 0, 0, "source released; all retained responses closed");
    }
    free(handle);
}
