/* Image-only cache: its HTTP context never shares installer sockets/state. */
#include "phstore_image_cache.h"
#include "phstore_catalog.h"
#include "phstore_config.h"
#include "phstore_notification.h"
#include "phstore_thread.h"
#include "phstore_raw_image_http.h"
#include "phstore_assets.h"
#include "phstore_gdrive.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#define CACHE_DIR "/data/phstore2/resimler"
#define IMAGE_MAX (8u * 1024u * 1024u)
#define CACHE_MAX (UINT64_C(1024) * 1024 * 1024)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int running, pending, native_result;
static size_t total, downloaded, existing, failed, missing;
static uint64_t bytes_written;
static char phase[24] = "idle";
#define IMAGE_LOG "/data/phstore2/logs/image-cache.log"
#define IMAGE_LOG_MAX (4u * 1024u * 1024u)
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char last_stage[64] = "idle", last_host[256] = "";
static int last_http_status;
static void image_log(const char *event, int rc, const char *format, ...) {
    int saved_errno = errno;
    char detail[768], line[1200], stamp[40] = "time-unavailable";
    va_list args; va_start(args, format); vsnprintf(detail, sizeof(detail), format, args); va_end(args);
    struct timespec now; struct tm utc;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && gmtime_r(&now.tv_sec, &utc))
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    int count = snprintf(line, sizeof(line), "%s build=%s stage=%s rc=0x%08X errno=%d %s\n",
        stamp, PHSTORE_BUILD_ID, event, (unsigned)rc, saved_errno, detail);
    pthread_mutex_lock(&log_mutex);
    (void)mkdir("/data/phstore2", 0700); (void)mkdir("/data/phstore2/logs", 0700);
    struct stat st;
    if (stat(IMAGE_LOG, &st) == 0 && st.st_size >= IMAGE_LOG_MAX)
        (void)rename(IMAGE_LOG, "/data/phstore2/logs/image-cache.previous.log");
    int fd = open(IMAGE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0 && count > 0 && (size_t)count < sizeof(line)) {
        size_t used = 0;
        while (used < (size_t)count) {
            ssize_t n = write(fd, line + used, (size_t)count - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            used += (size_t)n;
        }
        if (rc < 0 || !strcmp(event, "RUN_DONE")) (void)fsync(fd);
    }
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&log_mutex);
    errno = saved_errno;
}
static void image_stage(const char *stage) {
    pthread_mutex_lock(&mutex);
    snprintf(last_stage, sizeof(last_stage), "%s", stage);
    pthread_mutex_unlock(&mutex);
    image_log(stage, 0, "call=begin");
    errno = 0;
}
static void image_endpoint(const char *url) {
    const char *host = strstr(url, "://"); host = host ? host + 3 : url;
    size_t n = strcspn(host, "/?#");
    if (n >= sizeof(last_host)) n = sizeof(last_host)-1;
    pthread_mutex_lock(&mutex);
    memcpy(last_host, host, n); last_host[n] = '\0';
    pthread_mutex_unlock(&mutex);
    image_log("ENDPOINT", 0, "host=%.*s scheme=%s", (int)n, host, !strncmp(url, "https:", 6) ? "https" : "http");
}
char *phstore_image_cache_log_text(size_t *length) {
    *length = 0;
    pthread_mutex_lock(&log_mutex);
    int fd = open(IMAGE_LOG, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > IMAGE_LOG_MAX + 1200) {
        if (fd >= 0) close(fd);
        pthread_mutex_unlock(&log_mutex); return NULL;
    }
    size_t capacity = (size_t)st.st_size; char *body = malloc(capacity + 1);
    while (body && *length < capacity) {
        ssize_t n = read(fd, body + *length, capacity - *length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        *length += (size_t)n;
    }
    close(fd); pthread_mutex_unlock(&log_mutex);
    if (body) body[*length] = '\0';
    return body;
}
static uint64_t milliseconds(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void image_key(const char *url, char key[33]) {
    /* Non-cryptographic URL fingerprints; no URL text in filesystem paths. */
    uint64_t a = UINT64_C(14695981039346656037), b = UINT64_C(7809847782465536322);
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        a = (a ^ *p) * UINT64_C(1099511628211);
        b = (b ^ *p) * UINT64_C(14029467366897019727);
    }
    snprintf(key, 33, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
}
static const char *image_type(const unsigned char *p, size_t n) {
    if (n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 4 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) return "image/jpeg";
    if (n >= 13 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) return "image/gif";
    if (n >= 16 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return "image/webp";
    return NULL;
}
static int cached(const char *key, size_t *length, const char **mime) {
    char path[128]; snprintf(path, sizeof(path), CACHE_DIR "/%s.img", key);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st; unsigned char header[32];
    ssize_t n = read(fd, header, sizeof(header));
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= IMAGE_MAX;
    close(fd);
    const char *type = n > 0 ? image_type(header, (size_t)n) : NULL;
    if (!ok || !type) return 0;
    if (length) *length = (size_t)st.st_size;
    if (mime) *mime = type;
    return 1;
}
unsigned char *phstore_image_cache_read(const char *key, size_t *length, const char **mime) {
    if (strlen(key) != 32) return NULL;
    for (int i = 0; i < 32; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) return NULL;
    if (!cached(key, length, mime)) return NULL;
    char path[128]; snprintf(path, sizeof(path), CACHE_DIR "/%s.img", key);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    unsigned char *data = malloc(*length);
    size_t used = 0;
    while (data && used < *length) {
        ssize_t n = read(fd, data + used, *length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (used != *length) { free(data); return NULL; }
    return data;
}
static uint64_t disk_usage(void) {
    DIR *dir = opendir(CACHE_DIR); if (!dir) return 0;
    struct dirent *entry; uint64_t used = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[512]; struct stat st;
        snprintf(path, sizeof(path), CACHE_DIR "/%s", entry->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) used += (uint64_t)st.st_size;
    }
    closedir(dir); return used;
}

static int is_ph_ps5_cover(const char *url) {
    size_t n=strlen(url);
    for(size_t i=0;i<phstore_embedded_asset_count;i++) {
        const phstore_embedded_asset_t *a=&phstore_embedded_assets[i];
        if(strcmp(a->path,"/metadata/ph-ps5-covers.txt"))continue;
        size_t start=0;
        for(size_t k=0;k<a->size;k++)if(a->data[k]=='\n') {
            if(k-start==n && !memcmp(a->data+start,url,n))return 1;start=k+1;
        }
    }
    return 0;
}
static int fetch_ph_ps5_cover(const char *key,const char *url,uint64_t *size,int *rc) {
    char temporary[128],destination[128];unsigned char signature[32];int status=0;
    snprintf(temporary,sizeof(temporary),CACHE_DIR "/%s.part",key);
    snprintf(destination,sizeof(destination),CACHE_DIR "/%s.img",key);
    image_endpoint(url);image_stage("PH_PS5_NATIVE_HTTPS");
    *rc=phstore_public_cover_fetch(url,temporary,IMAGE_MAX,size,&status);
    pthread_mutex_lock(&mutex);last_http_status=status;if(status==404)missing++;pthread_mutex_unlock(&mutex);
    image_log("PH_PS5_HTTPS_RESULT",*rc,"status=%d bytes=%llu key=%s",status,(unsigned long long)*size,key);
    if(*rc)return -1;
    int fd=open(temporary,O_RDONLY);ssize_t n=fd<0?-1:read(fd,signature,sizeof(signature));if(fd>=0)close(fd);
    if(n<0 || !image_type(signature,(size_t)n)){unlink(temporary);*rc=-EINVAL;return -1;}
    if(rename(temporary,destination)){*rc=-errno;unlink(temporary);return -1;}
    image_log("IMAGE_DONE",0,"key=%s bytes=%llu source=ph-ps5-https",key,(unsigned long long)*size);return 0;
}
static int fetch_image(const char *key, uint64_t *size, int *rc) {
    phstore_raw_image_response_t response = {.fd = -1};
    int file = -1, result = -1;
    uint64_t expected = 0;
    char temporary[128], destination[128], url[160];
    snprintf(temporary, sizeof(temporary), CACHE_DIR "/%s.part", key);
    snprintf(destination, sizeof(destination), CACHE_DIR "/%s.img", key);
    snprintf(url, sizeof(url), PHSTORE_COVER_BASE_URL "%s.img", key);
    *size = 0;
    uint64_t deadline = milliseconds() + 60000;
    image_endpoint(url);
    image_stage("RAW_HTTP_OPEN");
    image_log("VDS_REQUEST", 0, "url=%s original_url_fallback=disabled", url);
    int opened = phstore_raw_image_open(key, IMAGE_MAX, &response);
    int open_errno = opened == 0 ? 0 : (errno ? errno : EIO);
    pthread_mutex_lock(&mutex);
    last_http_status = response.status;
    if (response.status == 404) missing++;
    pthread_mutex_unlock(&mutex);
    *rc = opened == 0 ? 0 : (response.status && response.status != 200 ? -response.status : -open_errno);
    image_log("RAW_HTTP_OPEN", *rc, "call=end transport_stage=%s status=%d error=%d",
        response.stage ? response.stage : "unknown", response.status, open_errno);
    image_log("HTTP_STATUS", response.status == 200 ? 0 : *rc, "status=%d", response.status);
    if (response.status == 404) image_log("VDS_MISSING", -404, "key=%s fallback=disabled", key);
    if (opened != 0) goto done;
    expected = response.content_length;
    image_log("CONTENT_LENGTH", 0, "expected=%llu source=vds-http", (unsigned long long)expected);
    image_stage("FILE_OPEN");
    file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    image_log("FILE_OPEN", file < 0 ? -errno : 0, "path=%s", temporary);
    if (file < 0) { *rc = -errno; goto done; }
    unsigned char buffer[32768], signature[32]; size_t signature_length = 0;
    *size = 0;
    image_stage("RAW_HTTP_READ");
    for (;;) {
        if (milliseconds() > deadline) { *rc = -ETIMEDOUT; goto done; }
        int n = phstore_raw_image_read(&response, buffer, sizeof(buffer));
        if (n < 0) { *rc = -(errno ? errno : EIO); image_log("BODY_READ_ERROR", *rc, "received=%llu", (unsigned long long)*size); goto done; }
        if (!n) break;
        if ((size_t)n > sizeof(buffer) || *size + (unsigned)n > IMAGE_MAX) { *rc = -EFBIG; goto done; }
        if (signature_length < sizeof(signature)) {
            size_t take = sizeof(signature) - signature_length;
            if (take > (size_t)n) take = (size_t)n;
            memcpy(signature + signature_length, buffer, take); signature_length += take;
        }
        size_t used = 0;
        while (used < (size_t)n) {
            ssize_t written = write(file, buffer + used, (size_t)n - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { *rc = -(errno ? errno : EIO); goto done; }
            used += (size_t)written;
        }
        *size += (unsigned)n;
    }
    image_log("BODY_DONE", 0, "bytes=%llu expected=%llu signature=%02x%02x%02x%02x type=%s",
        (unsigned long long)*size, (unsigned long long)expected,
        signature_length > 0 ? signature[0] : 0, signature_length > 1 ? signature[1] : 0,
        signature_length > 2 ? signature[2] : 0, signature_length > 3 ? signature[3] : 0,
        image_type(signature, signature_length) ? image_type(signature, signature_length) : "unsupported");
    image_stage("IMAGE_VALIDATE");
    if (!image_type(signature, signature_length) || (expected != *size)) { *rc = -EINVAL; goto done; }
    image_stage("FILE_SYNC");
    if (fsync(file) != 0) { *rc = -errno; goto done; }
    int closed = close(file); file = -1;
    if (closed != 0) { *rc = -(errno ? errno : EIO); goto done; }
    image_stage("FILE_RENAME");
    if (rename(temporary, destination) != 0) { *rc = -errno; goto done; }
    *rc = 0; result = 0;
done:
    image_log(result == 0 ? "IMAGE_DONE" : "IMAGE_FAILED", result == 0 ? 0 : (*rc ? *rc : -EIO),
        "key=%s bytes=%llu", key, (unsigned long long)*size);
    if (file >= 0) close(file);
    phstore_raw_image_close(&response);
    if (result != 0) { unlink(temporary); if (*rc >= 0 && *rc < 100) *rc = -EIO; }
    return result;
}
static void free_urls(char **urls, size_t count) {
    for (size_t i = 0; i < count; i++) free(urls[i]);
    free(urls);
}
static void *cache_worker(void *unused) {
    (void)unused;
    for (;;) {
        size_t count = 0; char **urls = phstore_catalog_cover_urls(&count);
        pthread_mutex_lock(&mutex);
        total = count; downloaded = existing = failed = missing = 0; bytes_written = 0; native_result = 0;
        snprintf(phase, sizeof(phase), "downloading");
        pthread_mutex_unlock(&mutex);
        image_log("RUN_BEGIN", 0, "total=%zu catalog_snapshot=%s source=vds-http base=" PHSTORE_COVER_BASE_URL, count, urls ? "ok" : "missing");
        int directory_ok = mkdir(CACHE_DIR, 0700) == 0 || errno == EEXIST;
        image_log("CACHE_DIRECTORY", directory_ok ? 0 : -errno, "path=%s", CACHE_DIR);
        uint64_t usage = disk_usage();
        for (size_t i = 0; urls && i < count; i++) {
            char key[33]; image_key(urls[i], key);
            image_log("ITEM_BEGIN", 0, "item=%zu/%zu key=%s", i+1, count, key);
            pthread_mutex_lock(&mutex); last_http_status = 0; pthread_mutex_unlock(&mutex);
            if (cached(key, NULL, NULL)) { image_log("CACHE_HIT", 0, "key=%s", key); pthread_mutex_lock(&mutex); existing++; pthread_mutex_unlock(&mutex); continue; }
            if (!directory_ok) { pthread_mutex_lock(&mutex); failed++; native_result = -EIO; pthread_mutex_unlock(&mutex); continue; }
            uint64_t size = 0; int outcome = -1, image_rc = 0;
            struct statvfs fs;
            int fs_result = statvfs(CACHE_DIR, &fs);
            int fs_errno = fs_result == 0 ? 0 : errno;
            uint64_t free_bytes = fs_result == 0 ? (uint64_t)fs.f_bavail * fs.f_frsize : 0;
            int space_ok = usage + IMAGE_MAX <= CACHE_MAX && fs_result == 0 && free_bytes > IMAGE_MAX + 64u * 1024u * 1024u;
            image_log("SPACE_CHECK", space_ok ? 0 : -(fs_errno ? fs_errno : ENOSPC), "used=%llu free=%llu statvfs=%d fs_errno=%d",
                (unsigned long long)usage, (unsigned long long)free_bytes, fs_result, fs_errno);
            if (space_ok) outcome = is_ph_ps5_cover(urls[i]) ? fetch_ph_ps5_cover(key,urls[i],&size,&image_rc) : fetch_image(key, &size, &image_rc);
            else image_rc = -(fs_errno ? fs_errno : ENOSPC);
            pthread_mutex_lock(&mutex);
            if (outcome == 0) { downloaded++; bytes_written += size; usage += size; }
            else { failed++; native_result = image_rc; }
            pthread_mutex_unlock(&mutex);
            image_log("ITEM_DONE", outcome == 0 ? 0 : image_rc, "item=%zu/%zu bytes=%llu", i+1, count, (unsigned long long)size);
            fprintf(stderr, "[PHSTORE/IMAGES] item=%zu/%zu rc=0x%08x bytes=%llu\n", i+1, count, (unsigned)image_rc, (unsigned long long)size);
        }
        int snapshot_ok = urls != NULL;
        free_urls(urls, count);
        pthread_mutex_lock(&mutex);
        if (pending) { pending = 0; pthread_mutex_unlock(&mutex); continue; }
        int success = snapshot_ok && directory_ok && failed == 0;
        snprintf(phase, sizeof(phase), "%s", success ? "ready" : "partial");
        if (!snapshot_ok) native_result = -EIO;
        size_t errors = failed, saved_count = downloaded, hit_count = existing, missing_count = missing;
        pthread_mutex_unlock(&mutex);
        image_log("RUN_DONE", success ? 0 : -EIO, "downloaded=%zu existing=%zu failed=%zu missing=%zu", saved_count, hit_count, errors, missing_count);
        if (success) phstore_notify("Cache hazır, kapatıp açın");
        else phstore_notify("Görsel cache tamamlanamadı (%zu hata). Ayarlar'dan tekrar deneyin.", errors ? errors : 1);
        pthread_mutex_lock(&mutex);
        if (pending) { pending = 0; pthread_mutex_unlock(&mutex); continue; }
        running = 0;
        pthread_mutex_unlock(&mutex);
        return NULL;
    }
}
int phstore_image_cache_start(int automatic) {
    phstore_catalog_status_t catalog; phstore_catalog_get_status(&catalog);
    if (!catalog.has_catalog) return -1;
    pthread_mutex_lock(&mutex);
    if (running) { if (automatic) pending = 1; pthread_mutex_unlock(&mutex); return 1; }
    image_log("START_REQUEST", 0, "automatic=%d", automatic);
    running = 1; pending = 0; total = downloaded = existing = failed = missing = 0; native_result = 0;
    snprintf(phase, sizeof(phase), "starting");
    pthread_t thread;
    int rc = phstore_thread_create(&thread, cache_worker, NULL);
    if (rc != 0) { image_log("WORKER_CREATE_FAILED", rc, "pthread_create"); running = 0; native_result = rc; snprintf(phase, sizeof(phase), "error"); }
    else pthread_detach(thread);
    pthread_mutex_unlock(&mutex);
    return rc == 0 ? 0 : -1;
}
char *phstore_image_cache_status_json(void) {
    char *body = malloc(1024); if (!body) return NULL;
    pthread_mutex_lock(&mutex);
    snprintf(body, 384, "{\"ok\":true,\"running\":%s,\"state\":\"%s\",\"total\":%zu,\"downloaded\":%zu,\"existing\":%zu,\"failed\":%zu,\"bytes\":%llu,\"native_result\":%d}",
        running ? "true" : "false", phase, total, downloaded, existing, failed, (unsigned long long)bytes_written, native_result);
    size_t used = strlen(body);
    if (used && body[used-1] == '}') snprintf(body + used - 1, 1024 - used + 1,
        ",\"missing\":%zu,\"source\":\"vds-http\",\"last_stage\":\"%s\",\"last_host\":\"%s\",\"http_status\":%d,\"log_path\":\"/data/phstore2/logs/image-cache.log\"}",
        missing, last_stage, last_host, last_http_status);
    pthread_mutex_unlock(&mutex); return body;
}
char *phstore_image_cache_index_json(void) {
    size_t count = 0; char **urls = phstore_catalog_cover_urls(&count);
    if (!urls) return NULL;
    size_t capacity = 32;
    for (size_t i = 0; i < count; i++) capacity += strlen(urls[i]) * 2 + 80;
    char *body = malloc(capacity);
    if (!body) { free_urls(urls, count); return NULL; }
    size_t used = (size_t)snprintf(body, capacity, "{\"ok\":true,\"items\":[");
    int first = 1;
    for (size_t i = 0; i < count; i++) {
        char key[33]; image_key(urls[i], key);
        if (!cached(key, NULL, NULL)) continue;
        used += (size_t)snprintf(body + used, capacity - used, "%s{\"url\":\"", first ? "" : ","); first = 0;
        for (const char *p = urls[i]; *p; p++) { if (*p == '"' || *p == '\\') body[used++] = '\\'; body[used++] = *p; }
        used += (size_t)snprintf(body + used, capacity - used, "\",\"path\":\"/resimler/%s\"}", key);
    }
    snprintf(body + used, capacity - used, "]}");
    free_urls(urls, count); return body;
}
